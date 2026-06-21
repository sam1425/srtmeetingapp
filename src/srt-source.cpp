#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>
#include "srt-source.h"
#include "srt-broker.h"

#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

static std::mutex s_buffer_registry_mutex;
static std::map<std::string, std::shared_ptr<PacketBuffer>> s_buffer_registry;

void register_participant_buffer(const std::string &name, std::shared_ptr<PacketBuffer> buffer)
{
	std::lock_guard<std::mutex> lock(s_buffer_registry_mutex);
	s_buffer_registry[name] = buffer;
}

void unregister_participant_buffer(const std::string &name)
{
	std::lock_guard<std::mutex> lock(s_buffer_registry_mutex);
	s_buffer_registry.erase(name);
}

static std::shared_ptr<PacketBuffer> get_participant_buffer(const std::string &name)
{
	std::lock_guard<std::mutex> lock(s_buffer_registry_mutex);
	auto it = s_buffer_registry.find(name);
	if (it != s_buffer_registry.end()) {
		return it->second;
	}
	return nullptr;
}

struct SRTSourceData {
	obs_source_t *source;
	std::string participant_name;
	std::shared_ptr<PacketBuffer> buffer;

	// ffmpeg state
	AVFormatContext *fmt_ctx{nullptr};
	AVIOContext *avio_ctx{nullptr};
	uint8_t *avio_buffer{nullptr};
	AVCodecContext *video_dec_ctx{nullptr};
	AVCodecContext *audio_dec_ctx{nullptr};
	int video_stream_idx{-1};
	int audio_stream_idx{-1};
	SwsContext *sws_ctx{nullptr};
	SwrContext *swr_ctx{nullptr};

	// Internal staging buffer for AVIO reads
	std::vector<uint8_t> staging;
	size_t staging_offset{0};

	// Timestamp tracking
	int64_t first_video_pts{AV_NOPTS_VALUE};
	uint64_t first_video_obs_ts{0};
	int64_t first_audio_pts{AV_NOPTS_VALUE};
	uint64_t first_audio_obs_ts{0};

	// Decode thread
	std::unique_ptr<std::thread> decode_thread;
	std::atomic<bool> running{false};
};

static int avio_read_callback(void *opaque, uint8_t *buf, int buf_size)
{
	SRTSourceData *data = static_cast<SRTSourceData *>(opaque);
	if (!data || !data->running)
		return AVERROR_EOF;

	// 1. Drain leftover staging data first
	if (data->staging_offset < data->staging.size()) {
		size_t avail = data->staging.size() - data->staging_offset;
		size_t copy_len = (avail < (size_t)buf_size) ? avail : (size_t)buf_size;
		std::memcpy(buf, data->staging.data() + data->staging_offset, copy_len);
		data->staging_offset += copy_len;
		if (data->staging_offset >= data->staging.size()) {
			data->staging.clear();
			data->staging_offset = 0;
		}
		return (int)copy_len;
	}

	// 2. Fetch new packet
	std::vector<uint8_t> pkt;
	while (data->running) {
		pkt = data->buffer->pop(100);
		if (!pkt.empty()) {
			break;
		}
		if (data->buffer->is_eof()) {
			return AVERROR_EOF;
		}
	}

	if (!data->running)
		return AVERROR_EOF;

	if (pkt.empty()) {
		return AVERROR(EAGAIN);
	}

	// 3. Copy to destination and stage any remainder
	if (pkt.size() <= (size_t)buf_size) {
		std::memcpy(buf, pkt.data(), pkt.size());
		return (int)pkt.size();
	} else {
		std::memcpy(buf, pkt.data(), buf_size);
		data->staging.assign(pkt.begin() + buf_size, pkt.end());
		data->staging_offset = 0;
		return buf_size;
	}
}

static void cleanup_ffmpeg(SRTSourceData *data)
{
	if (data->sws_ctx) {
		sws_freeContext(data->sws_ctx);
		data->sws_ctx = nullptr;
	}
	if (data->swr_ctx) {
		swr_free(&data->swr_ctx);
		data->swr_ctx = nullptr;
	}
	if (data->video_dec_ctx) {
		avcodec_free_context(&data->video_dec_ctx);
	}
	if (data->audio_dec_ctx) {
		avcodec_free_context(&data->audio_dec_ctx);
	}
	if (data->fmt_ctx) {
		avformat_close_input(&data->fmt_ctx);
	}
	if (data->avio_ctx) {
		// avio_ctx->buffer was allocated by av_malloc, free context
		data->avio_ctx->buffer = nullptr;
		avio_context_free(&data->avio_ctx);
	}
	if (data->avio_buffer) {
		av_freep(&data->avio_buffer);
	}
	data->video_stream_idx = -1;
	data->audio_stream_idx = -1;
}

static bool setup_ffmpeg(SRTSourceData *data)
{
	cleanup_ffmpeg(data);

	constexpr size_t AVIO_BUF_SIZE = 32768;
	data->avio_buffer = static_cast<uint8_t *>(av_malloc(AVIO_BUF_SIZE));
	if (!data->avio_buffer) {
		obs_log(LOG_ERROR, "Failed to allocate AVIO buffer for %s", data->participant_name.c_str());
		return false;
	}

	data->avio_ctx = avio_alloc_context(data->avio_buffer, AVIO_BUF_SIZE, 0, data, avio_read_callback, nullptr, nullptr);
	if (!data->avio_ctx) {
		obs_log(LOG_ERROR, "Failed to allocate AVIO context for %s", data->participant_name.c_str());
		return false;
	}

	data->fmt_ctx = avformat_alloc_context();
	if (!data->fmt_ctx) {
		obs_log(LOG_ERROR, "Failed to allocate format context for %s", data->participant_name.c_str());
		return false;
	}
	data->fmt_ctx->pb = data->avio_ctx;

	if (avformat_open_input(&data->fmt_ctx, nullptr, nullptr, nullptr) < 0) {
		obs_log(LOG_ERROR, "Failed to open input stream for %s", data->participant_name.c_str());
		return false;
	}

	if (avformat_find_stream_info(data->fmt_ctx, nullptr) < 0) {
		obs_log(LOG_ERROR, "Failed to find stream info for %s", data->participant_name.c_str());
		return false;
	}

	obs_log(LOG_INFO, "Stream info for %s: %d streams, video_idx=%d, audio_idx=%d",
		data->participant_name.c_str(),
		data->fmt_ctx->nb_streams,
		av_find_best_stream(data->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0),
		av_find_best_stream(data->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0));

	data->video_stream_idx = av_find_best_stream(data->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	data->audio_stream_idx = av_find_best_stream(data->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

	if (data->video_stream_idx >= 0) {
		AVStream *vstream = data->fmt_ctx->streams[data->video_stream_idx];
		const AVCodec *vcodec = avcodec_find_decoder(vstream->codecpar->codec_id);
		if (vcodec) {
			data->video_dec_ctx = avcodec_alloc_context3(vcodec);
			if (data->video_dec_ctx) {
				avcodec_parameters_to_context(data->video_dec_ctx, vstream->codecpar);
				if (avcodec_open2(data->video_dec_ctx, vcodec, nullptr) < 0) {
					obs_log(LOG_ERROR, "Failed to open video codec for %s", data->participant_name.c_str());
					avcodec_free_context(&data->video_dec_ctx);
				}
			}
		}
	}

	if (data->audio_stream_idx >= 0) {
		AVStream *astream = data->fmt_ctx->streams[data->audio_stream_idx];
		const AVCodec *acodec = avcodec_find_decoder(astream->codecpar->codec_id);
		if (acodec) {
			data->audio_dec_ctx = avcodec_alloc_context3(acodec);
			if (data->audio_dec_ctx) {
				avcodec_parameters_to_context(data->audio_dec_ctx, astream->codecpar);
				if (avcodec_open2(data->audio_dec_ctx, acodec, nullptr) < 0) {
					obs_log(LOG_ERROR, "Failed to open audio codec for %s", data->participant_name.c_str());
					avcodec_free_context(&data->audio_dec_ctx);
				}
			}
		}
	}

	if (!data->video_dec_ctx && !data->audio_dec_ctx) {
		obs_log(LOG_ERROR, "Failed to find any valid audio/video decoder for %s", data->participant_name.c_str());
		return false;
	}

	return true;
}

static void decode_thread_func(SRTSourceData *data)
{
	obs_log(LOG_INFO, "Starting decode thread for %s", data->participant_name.c_str());

	if (!setup_ffmpeg(data)) {
		obs_log(LOG_ERROR, "Setup FFmpeg failed for %s", data->participant_name.c_str());
		return;
	}

	AVFrame *frame = av_frame_alloc();
	AVPacket *pkt = av_packet_alloc();

	while (data->running) {
		int ret = av_read_frame(data->fmt_ctx, pkt);
		if (ret < 0) {
			if (ret == AVERROR_EOF) {
				obs_log(LOG_INFO, "FFmpeg reached EOF for %s", data->participant_name.c_str());
			} else if (ret == AVERROR(EAGAIN)) {
				continue;
			} else {
				obs_log(LOG_WARNING, "av_read_frame error for %s: %d", data->participant_name.c_str(), ret);
			}
			break;
		}
		obs_log(LOG_DEBUG, "Read packet: stream=%d size=%d pts=%ld", pkt->stream_index, pkt->size, pkt->pts);

		if (pkt->stream_index == data->video_stream_idx && data->video_dec_ctx) {
			if (avcodec_send_packet(data->video_dec_ctx, pkt) >= 0) {
				while (avcodec_receive_frame(data->video_dec_ctx, frame) >= 0) {
					if (frame->format == AV_PIX_FMT_YUV420P) {
						obs_source_frame obs_frame{};
						obs_frame.width = frame->width;
						obs_frame.height = frame->height;
						obs_frame.format = VIDEO_FORMAT_I420;
						obs_frame.data[0] = frame->data[0];
						obs_frame.data[1] = frame->data[1];
						obs_frame.data[2] = frame->data[2];
						obs_frame.linesize[0] = frame->linesize[0];
						obs_frame.linesize[1] = frame->linesize[1];
						obs_frame.linesize[2] = frame->linesize[2];

						if (data->first_video_pts == AV_NOPTS_VALUE) {
							data->first_video_pts = frame->pts;
							data->first_video_obs_ts = os_gettime_ns();
						}
						int64_t pts_ns = av_rescale_q(frame->pts - data->first_video_pts,
										 data->fmt_ctx->streams[data->video_stream_idx]->time_base,
										 AVRational{1, 1000000000});
						obs_frame.timestamp = data->first_video_obs_ts + pts_ns;

						obs_source_output_video(data->source, &obs_frame);
						static int frame_count = 0;
						if (++frame_count % 300 == 0) {
							obs_log(LOG_DEBUG, "Output %d video frames for %s (%dx%d)", frame_count, data->participant_name.c_str(), frame->width, frame->height);
						}
					} else {
						if (!data->sws_ctx) {
							data->sws_ctx = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
										       frame->width, frame->height, AV_PIX_FMT_YUV420P,
										       SWS_BICUBIC, nullptr, nullptr, nullptr);
						}
						if (data->sws_ctx) {
							int dst_linesize[4];
							uint8_t *dst_data[4];
							if (av_image_alloc(dst_data, dst_linesize, frame->width, frame->height, AV_PIX_FMT_YUV420P, 1) >= 0) {
								sws_scale(data->sws_ctx, frame->data, frame->linesize, 0, frame->height, dst_data, dst_linesize);

								obs_source_frame obs_frame{};
								obs_frame.width = frame->width;
								obs_frame.height = frame->height;
								obs_frame.format = VIDEO_FORMAT_I420;
								obs_frame.data[0] = dst_data[0];
								obs_frame.data[1] = dst_data[1];
								obs_frame.data[2] = dst_data[2];
								obs_frame.linesize[0] = dst_linesize[0];
								obs_frame.linesize[1] = dst_linesize[1];
								obs_frame.linesize[2] = dst_linesize[2];

								if (data->first_video_pts == AV_NOPTS_VALUE) {
									data->first_video_pts = frame->pts;
									data->first_video_obs_ts = os_gettime_ns();
								}
								int64_t pts_ns = av_rescale_q(frame->pts - data->first_video_pts,
												 data->fmt_ctx->streams[data->video_stream_idx]->time_base,
												 AVRational{1, 1000000000});
								obs_frame.timestamp = data->first_video_obs_ts + pts_ns;

								obs_source_output_video(data->source, &obs_frame);
								av_freep(&dst_data[0]);
							}
						}
					}
				}
			}
		} else if (pkt->stream_index == data->audio_stream_idx && data->audio_dec_ctx) {
			if (avcodec_send_packet(data->audio_dec_ctx, pkt) >= 0) {
				while (avcodec_receive_frame(data->audio_dec_ctx, frame) >= 0) {
					int channels = frame->ch_layout.nb_channels;
					speaker_layout layout;
					switch (channels) {
					case 1: layout = SPEAKERS_MONO; break;
					case 2: layout = SPEAKERS_STEREO; break;
					case 3: layout = SPEAKERS_2POINT1; break;
					case 4: layout = SPEAKERS_4POINT0; break;
					case 5: layout = SPEAKERS_4POINT1; break;
					case 6: layout = SPEAKERS_5POINT1; break;
					case 8: layout = SPEAKERS_7POINT1; break;
					default: layout = SPEAKERS_STEREO; break;
					}

					if (!data->swr_ctx) {
						data->swr_ctx = swr_alloc();
						if (data->swr_ctx) {
							av_opt_set_chlayout(data->swr_ctx, "in_chlayout", &frame->ch_layout, 0);
							av_opt_set_chlayout(data->swr_ctx, "out_chlayout", &frame->ch_layout, 0);
							av_opt_set_int(data->swr_ctx, "in_sample_rate", frame->sample_rate, 0);
							av_opt_set_int(data->swr_ctx, "out_sample_rate", frame->sample_rate, 0);
							av_opt_set_sample_fmt(data->swr_ctx, "in_sample_fmt", (AVSampleFormat)frame->format, 0);
							av_opt_set_sample_fmt(data->swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
							swr_init(data->swr_ctx);
						}
					}

					if (data->swr_ctx) {
						uint8_t **output_samples = nullptr;
						int out_linesize = 0;
						int alloc_ret = av_samples_alloc_array_and_samples(&output_samples, &out_linesize,
															     channels, frame->nb_samples,
															     AV_SAMPLE_FMT_FLTP, 0);
						if (alloc_ret >= 0) {
							int convert_ret = swr_convert(data->swr_ctx, output_samples, frame->nb_samples,
											(const uint8_t **)frame->data, frame->nb_samples);
							if (convert_ret > 0) {
								obs_source_audio obs_audio{};
								obs_audio.frames = convert_ret;
								obs_audio.speakers = layout;
								obs_audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
								obs_audio.samples_per_sec = frame->sample_rate;
								for (int i = 0; i < channels; ++i) {
									obs_audio.data[i] = output_samples[i];
								}

								if (data->first_audio_pts == AV_NOPTS_VALUE) {
									data->first_audio_pts = frame->pts;
									data->first_audio_obs_ts = os_gettime_ns();
								}
								int64_t pts_ns = av_rescale_q(frame->pts - data->first_audio_pts,
												 data->fmt_ctx->streams[data->audio_stream_idx]->time_base,
												 AVRational{1, 1000000000});
								obs_audio.timestamp = data->first_audio_obs_ts + pts_ns;

								obs_source_output_audio(data->source, &obs_audio);
							}
							if (output_samples) {
								av_freep(&output_samples[0]);
								free(output_samples);
							}
						}
					}
				}
			}
		}

		av_packet_unref(pkt);
	}

	av_frame_free(&frame);
	av_packet_free(&pkt);
	cleanup_ffmpeg(data);

	obs_log(LOG_INFO, "Decode thread exited for %s", data->participant_name.c_str());
}

static const char *srt_participant_get_name(void *)
{
	return "SRT Participant";
}

static void *srt_participant_create(obs_data_t *settings, obs_source_t *source)
{
	SRTSourceData *data = new SRTSourceData();
	data->source = source;
	data->participant_name = obs_data_get_string(settings, "participant_name");
	data->buffer = get_participant_buffer(data->participant_name);

	if (!data->buffer) {
		obs_log(LOG_WARNING, "No PacketBuffer registered yet for participant: %s", data->participant_name.c_str());
	} else {
		data->running = true;
		data->decode_thread = std::make_unique<std::thread>(decode_thread_func, data);
	}

	return data;
}

static void srt_participant_destroy(void *private_data)
{
	SRTSourceData *data = static_cast<SRTSourceData *>(private_data);
	if (data) {
		data->running = false;
		if (data->decode_thread && data->decode_thread->joinable()) {
			data->decode_thread->join();
		}
		cleanup_ffmpeg(data);
		delete data;
	}
}

static void srt_participant_update(void *private_data, obs_data_t *settings)
{
	SRTSourceData *data = static_cast<SRTSourceData *>(private_data);
	if (!data) return;

	std::string new_name = obs_data_get_string(settings, "participant_name");
	if (new_name != data->participant_name) {
		obs_log(LOG_INFO, "Updating srt_participant from %s to %s", data->participant_name.c_str(), new_name.c_str());
		
		data->running = false;
		if (data->decode_thread && data->decode_thread->joinable()) {
			data->decode_thread->join();
			data->decode_thread.reset();
		}

		cleanup_ffmpeg(data);
		data->first_video_pts = AV_NOPTS_VALUE;
		data->first_audio_pts = AV_NOPTS_VALUE;
		data->staging.clear();
		data->staging_offset = 0;

		data->participant_name = new_name;
		data->buffer = get_participant_buffer(new_name);

		if (data->buffer) {
			data->running = true;
			data->decode_thread = std::make_unique<std::thread>(decode_thread_func, data);
		}
	}
}

static void srt_participant_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "participant_name", "");
}

static struct obs_source_info srt_participant_info = {};

void register_srt_participant_source()
{
	srt_participant_info.id = "srt_participant";
	srt_participant_info.type = OBS_SOURCE_TYPE_INPUT;
	srt_participant_info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO;
	srt_participant_info.get_name = srt_participant_get_name;
	srt_participant_info.create = srt_participant_create;
	srt_participant_info.destroy = srt_participant_destroy;
	srt_participant_info.update = srt_participant_update;
	srt_participant_info.get_defaults = srt_participant_get_defaults;
	obs_register_source(&srt_participant_info);
}
