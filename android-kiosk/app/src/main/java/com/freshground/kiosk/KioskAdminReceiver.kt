package com.freshground.kiosk

import android.app.admin.DeviceAdminReceiver

/** Device-owner receiver. Provision once with:
 *  adb shell dpm set-device-owner com.freshground.kiosk/.KioskAdminReceiver */
class KioskAdminReceiver : DeviceAdminReceiver()
