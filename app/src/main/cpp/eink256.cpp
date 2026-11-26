
#include <jni.h>
#include <android/bitmap.h>
#include <android/log.h>

#include <cmath>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <new>

// 日志标签
#define LOG_TAG "zyymeEink256"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// 限制灰度值在 0-255
#define CLAMP(val) ((val) < 0 ? 0 : ((val) > 255 ? 255 : (val)))

// ============================================================
// RGBA_8888 像素处理
// ============================================================

struct Pixel8888 {

    static int getGray(uint32_t color) {
        int r = (color >> 16) & 0xFF;
        int g = (color >> 8) & 0xFF;
        int b = color & 0xFF;

        return (77 * r + 150 * g + 29 * b) >> 8;
    }

    static uint32_t pack(uint32_t original, int grayVal) {
        uint32_t alpha = original & 0xFF000000;

        return alpha |
               (static_cast<uint32_t>(grayVal) << 16) |
               (static_cast<uint32_t>(grayVal) << 8) |
               static_cast<uint32_t>(grayVal);
    }
};

// ============================================================
// RGB_565 像素处理
// ============================================================

struct Pixel565 {

    static int getGray(uint16_t color) {
        int r5 = (color >> 11) & 0x1F;
        int g6 = (color >> 5) & 0x3F;
        int b5 = color & 0x1F;

        int r8 = (r5 << 3) | (r5 >> 2);
        int g8 = (g6 << 2) | (g6 >> 4);
        int b8 = (b5 << 3) | (b5 >> 2);

        return (77 * r8 + 150 * g8 + 29 * b8) >> 8;
    }

    static uint16_t pack(uint16_t /* original */, int grayVal) {
        int r5 = grayVal >> 3;
        int g6 = grayVal >> 2;
        int b5 = grayVal >> 3;

        return static_cast<uint16_t>(
            (r5 << 11) | (g6 << 5) | b5
        );
    }
};

// ============================================================
// Floyd-Steinberg 16 阶灰度抖动算法
// ============================================================

template <typename T, typename Handler>
void applyDitherTemplate(
        void* pixelsRaw,
        int width,
        int height,
        int strideBytes) {

    uint8_t* base = static_cast<uint8_t*>(pixelsRaw);

    // 每像素字节数
    const int bytesPerPixel = sizeof(T);

    // 16 阶灰度，步长 17
    const int STEP = 17;

    // 当前行和下一行误差缓冲
    std::vector<int> currRowErr(width, 0);
    std::vector<int> nextRowErr(width, 0);

    for (int y = 0; y < height; ++y) {

        // 根据 stride 定位当前行
        T* row = reinterpret_cast<T*>(
            base + static_cast<size_t>(y) * strideBytes
        );

        for (int x = 0; x < width; ++x) {

            T originalColor = row[x];

            // 1. 获取灰度
            int gray = Handler::getGray(originalColor);

            // 2. 加入误差
            gray += currRowErr[x];

            // 3. 量化到 16 阶灰度
            int newGray = static_cast<int>(
                std::round(static_cast<float>(gray) / STEP)
            ) * STEP;

            newGray = CLAMP(newGray);

            // 4. 计算量化误差
            int quantError = gray - newGray;

            // 5. Floyd-Steinberg 误差扩散

            // 右侧 7/16
            if (x + 1 < width) {
                currRowErr[x + 1] += quantError * 7 / 16;
            }

            if (y + 1 < height) {

                // 左下 3/16
                if (x > 0) {
                    nextRowErr[x - 1] += quantError * 3 / 16;
                }

                // 正下方 5/16
                nextRowErr[x] += quantError * 5 / 16;

                // 右下 1/16
                if (x + 1 < width) {
                    nextRowErr[x + 1] += quantError / 16;
                }
            }

            // 6. 写回量化后的灰度
            row[x] = Handler::pack(originalColor, newGray);
        }

        // 交换误差缓冲
        currRowErr.swap(nextRowErr);

        // 清空下一行缓冲
        std::fill(nextRowErr.begin(), nextRowErr.end(), 0);
    }
}

// ============================================================
// JNI 接口
// ============================================================

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_zyyme_eink256_Eink256Native_ditherBitmap(
        JNIEnv* env,
        jobject thiz,
        jobject bitmap) {

    if (bitmap == nullptr) {
        LOGE("Bitmap is null");
        return JNI_FALSE;
    }

    AndroidBitmapInfo info = {};

    // 获取 Bitmap 信息
    int result = AndroidBitmap_getInfo(env, bitmap, &info);

    if (result != ANDROID_BITMAP_RESULT_SUCCESS) {
        LOGE("AndroidBitmap_getInfo failed: %d", result);
        return JNI_FALSE;
    }

    if (info.width == 0 || info.height == 0) {
        LOGE("Invalid Bitmap size");
        return JNI_FALSE;
    }

    // 根据格式确定像素字节数
    int bytesPerPixel = 0;

    if (info.format == ANDROID_BITMAP_FORMAT_RGBA_8888) {
        bytesPerPixel = 4;
    } else if (info.format == ANDROID_BITMAP_FORMAT_RGB_565) {
        bytesPerPixel = 2;
    } else {
        LOGE("Unsupported Bitmap format: %d", info.format);
        return JNI_FALSE;
    }

    // 验证 stride
    if (info.stride < info.width * bytesPerPixel) {
        LOGE("Invalid Bitmap stride: %u", info.stride);
        return JNI_FALSE;
    }

    void* pixels = nullptr;

    // 锁定 Bitmap 像素
    result = AndroidBitmap_lockPixels(env, bitmap, &pixels);

    if (result != ANDROID_BITMAP_RESULT_SUCCESS || pixels == nullptr) {
        LOGE("AndroidBitmap_lockPixels failed: %d", result);
        return JNI_FALSE;
    }

    bool success = false;

    LOGI(
        "Dithering Bitmap: W=%u H=%u Format=%u Stride=%u",
        info.width,
        info.height,
        info.format,
        info.stride
    );

    try {

        if (info.format == ANDROID_BITMAP_FORMAT_RGBA_8888) {

            applyDitherTemplate<uint32_t, Pixel8888>(
                pixels,
                static_cast<int>(info.width),
                static_cast<int>(info.height),
                static_cast<int>(info.stride)
            );

            success = true;

        } else if (info.format == ANDROID_BITMAP_FORMAT_RGB_565) {

            applyDitherTemplate<uint16_t, Pixel565>(
                pixels,
                static_cast<int>(info.width),
                static_cast<int>(info.height),
                static_cast<int>(info.stride)
            );

            success = true;
        }

    } catch (const std::bad_alloc&) {

        LOGE("Memory allocation failed during dithering");

    } catch (...) {

        LOGE("Unknown error during dithering");
    }

    // 无论处理是否成功，只要锁定成功就必须解锁
    AndroidBitmap_unlockPixels(env, bitmap);

    if (success) {
        LOGI("Dithering completed successfully");
        return JNI_TRUE;
    }

    LOGE("Dithering failed");

    return JNI_FALSE;
}
