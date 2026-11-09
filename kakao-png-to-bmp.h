#ifndef KAKAO_PNG_TO_BMP_H
#define KAKAO_PNG_TO_BMP_H

#include <gio/gio.h>

GBytes *kakao_png_to_bmp(GBytes *png, GCancellable *cancel, GError **error);

#endif
