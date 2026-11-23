package launcher

import "core:image"
import "core:image/png"

save_png :: proc(pixels: [][4]u8, width, height: int, path: string) -> bool {
	img, ok := image.pixels_to_image(pixels, width, height)
	if !ok {return false}
	return png.save_to_file(path, &img) == nil
}
