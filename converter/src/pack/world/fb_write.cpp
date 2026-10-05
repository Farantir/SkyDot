// SPDX-License-Identifier: GPL-3.0-or-later
#include "fb_write.hpp"

namespace bethconv::pack::detail {

wfb::Vec3f to_fb(const record::Vec3& v) { return wfb::Vec3f(v.x, v.y, v.z); }

} // namespace bethconv::pack::detail
