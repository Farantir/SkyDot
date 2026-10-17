# SPDX-License-Identifier: GPL-3.0-or-later
#
# The image space the camera grades with, every frame: the weather's outside,
# the cell's inside, none with --image-space off, a neutral grade with
# --image-space neutral. Our shaders compute the fog themselves. Shell code
# both the viewer and the VR shell use.
class_name ImageSpaceSync
extends RefCounted


static func apply(viewport: Viewport, settings: ViewerSettings, session: GameSession,
		image_space: SkydotImageSpace) -> void:
	SkydotMaterials.sync_fog(viewport.find_world_3d().environment)
	if not settings.image_space:
		image_space.clear()
	else:
		var space: Dictionary
		# A sky seen from an interior ("show sky") does not grade the cell.
		if session.clock.has_weather() and not session.clock.weather.is_sky_only():
			space = session.clock.weather.get_image_space()
			if not settings.volumetric:
				space.erase("volumetric")
		else:
			space = session.place.interior_image_space.duplicate()
		if settings.neutral_grade:
			space.erase("cinematic")
			space.erase("tint")
		image_space.set_image_space(space)
