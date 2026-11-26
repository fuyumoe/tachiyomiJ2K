package com.zyyme.eink256

object Eink256Native {

    init {
        System.loadLibrary("eink256")
    }

    external fun ditherBitmap(bitmap: android.graphics.Bitmap): Boolean
}
