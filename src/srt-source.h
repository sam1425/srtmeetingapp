#pragma once

#include <memory>
#include <string>

class PacketBuffer;

/* Register the "srt_participant" source type with OBS.
   Call once from obs_module_load(). */
void register_srt_participant_source();

/* Buffer registry – broker registers a buffer before creating a source,
   and the source's create/update callback looks it up by participant name. */
void register_participant_buffer(const std::string &name,
				 std::shared_ptr<PacketBuffer> buffer);
void unregister_participant_buffer(const std::string &name);
