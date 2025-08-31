//
// Dagor Engine 6.5 - Game Libraries
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

class TMatrix;

namespace camtrack
{
typedef void *handle_t;
const handle_t INVALID_HANDLE = NULL;

void record(const char *filename);
void update_record(float abs_time, const TMatrix &itm, const float hor_fov);
void stop_record();

handle_t load_track(const char *filename);
bool get_track(handle_t handle, float abs_time, TMatrix &out_itm, float &out_fov);
void stop_play(handle_t handle);

void term();
}; // namespace camtrack
