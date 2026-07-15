#pragma once

struct m_config_cache;
struct m_sub_options;

// Shared gpu-next options registered by the standard vo_gpu_next backend.
extern const struct m_sub_options gl_next_conf;

// Keep the private gl_next_opts layout confined to vo_gpu_next.c.
int gpu_next_get_image_subs_hdr_peak(const struct m_config_cache *cache);
