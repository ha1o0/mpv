#ifndef MP_RECORDER_H_
#define MP_RECORDER_H_

#include <stdbool.h>

struct mp_recorder;
struct mpv_global;
struct demux_packet;
struct sh_stream;
struct demux_attachment;
struct mp_recorder_sink;

struct mp_recorder *mp_recorder_create(struct mpv_global *global,
                                       const char *target_file,
                                       struct sh_stream **streams,
                                       int num_streams,
                                       struct demux_attachment **demux_attachments,
                                       int num_attachments);
// Close and release the recorder; false means a mux/write operation failed.
bool mp_recorder_destroy(struct mp_recorder *r);
// A standalone, already aligned cache snapshot has a known timestamp origin.
void mp_recorder_set_timestamp_base(struct mp_recorder *r, double base);
void mp_recorder_mark_discontinuity(struct mp_recorder *r);

struct mp_recorder_sink *mp_recorder_get_sink(struct mp_recorder *r,
                                              struct sh_stream *stream);
void mp_recorder_feed_packet(struct mp_recorder_sink *s,
                             struct demux_packet *pkt);

#endif
