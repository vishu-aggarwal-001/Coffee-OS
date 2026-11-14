package com.freshground.kiosk

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/** Opens the kiosk after the tablet boots (it is also the home screen when provisioned). */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        context.startActivity(
            Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        )
    }
}
