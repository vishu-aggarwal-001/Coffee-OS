package com.freshground.kiosk

import android.annotation.SuppressLint
import android.app.Activity
import android.app.AlertDialog
import android.app.admin.DevicePolicyManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.BatteryManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.UserManager
import android.provider.Settings
import android.text.InputType
import android.view.MotionEvent
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.EditText
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors

/**
 * Full-screen kiosk for the coffee machine.
 *  - Shows the touchscreen menu that the controller serves at MACHINE_URL.
 *  - Locks the tablet to this app when provisioned as device owner (see README).
 *  - Reports battery level to the controller every minute so it can switch charging
 *    on at 40% and off at 80%, which stops the battery swelling in a warm cabinet.
 *  - Service exit: tap the top-left corner 7 times, then enter the owner PIN.
 */
class MainActivity : Activity() {

    private lateinit var web: WebView
    private val main = Handler(Looper.getMainLooper())
    private val io = Executors.newSingleThreadExecutor()
    private val machineUrl = BuildConfig.MACHINE_URL
    private var showingOffline = false
    private var cornerTaps = 0
    private var firstTapAt = 0L

    private val dpm by lazy { getSystemService(Context.DEVICE_POLICY_SERVICE) as DevicePolicyManager }
    private val admin by lazy { ComponentName(this, KioskAdminReceiver::class.java) }

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        web = WebView(this).apply {
            setBackgroundColor(0xFF0A3D27.toInt())
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            settings.mediaPlaybackRequiresUserGesture = false
            settings.setSupportZoom(false)
            settings.builtInZoomControls = false
            settings.textZoom = 100
            isHapticFeedbackEnabled = false
            isLongClickable = false
            setOnLongClickListener { true }
            webViewClient = object : WebViewClient() {
                override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
                    // Stay on the machine's pages; never open other sites
                    val host = request.url.host ?: return true
                    val machineHost = URL(machineUrl).host
                    return !(host == machineHost || request.url.scheme == "file")
                }
                override fun onReceivedError(view: WebView, request: WebResourceRequest, error: WebResourceError) {
                    if (request.isForMainFrame) showOffline()
                }
                override fun onPageFinished(view: WebView, url: String) {
                    if (url.startsWith("http")) showingOffline = false
                }
            }
        }
        setContentView(web)

        configureDeviceOwner()
        preferWifi()
        loadMachine()
        startBatteryReports()
    }

    override fun onResume() {
        super.onResume()
        hideSystemBars()
        if (dpm.isDeviceOwnerApp(packageName)) {
            try { startLockTask() } catch (_: Exception) { }
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    @Deprecated("Back is disabled in kiosk mode")
    override fun onBackPressed() { /* customers cannot leave the menu */ }

    // Hidden service gesture: 7 taps in the top-left corner within 4 seconds
    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        if (ev.action == MotionEvent.ACTION_DOWN && ev.x < 120 && ev.y < 120) {
            val now = System.currentTimeMillis()
            if (now - firstTapAt > 4000) { firstTapAt = now; cornerTaps = 0 }
            if (++cornerTaps >= 7) { cornerTaps = 0; askServicePin() }
        }
        return super.dispatchTouchEvent(ev)
    }

    private fun loadMachine() {
        showingOffline = false
        web.loadUrl(machineUrl)
    }

    private fun showOffline() {
        if (!showingOffline) {
            showingOffline = true
            web.loadUrl("file:///android_asset/offline.html")
        }
        main.postDelayed({ if (showingOffline) { showingOffline = false; web.loadUrl(machineUrl) } }, 5000)
    }

    private fun hideSystemBars() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.insetsController?.let {
                it.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                or View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_LAYOUT_STABLE or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN)
        }
    }

    /** When provisioned as device owner: lock to this app, act as the home screen, no lock screen. */
    private fun configureDeviceOwner() {
        if (!dpm.isDeviceOwnerApp(packageName)) return
        try {
            dpm.setLockTaskPackages(admin, arrayOf(packageName))
            dpm.setKeyguardDisabled(admin, true)
            dpm.setStatusBarDisabled(admin, true)
            dpm.addUserRestriction(admin, UserManager.DISALLOW_SAFE_BOOT)
            dpm.setGlobalSetting(admin, Settings.Global.STAY_ON_WHILE_PLUGGED_IN,
                (BatteryManager.BATTERY_PLUGGED_AC or BatteryManager.BATTERY_PLUGGED_USB).toString())
            val home = IntentFilter(Intent.ACTION_MAIN).apply {
                addCategory(Intent.CATEGORY_HOME); addCategory(Intent.CATEGORY_DEFAULT)
            }
            dpm.addPersistentPreferredActivity(admin, home, ComponentName(this, MainActivity::class.java))
        } catch (_: SecurityException) { }
    }

    /** The machine's Wi-Fi has no internet; make Android keep using it anyway. */
    private fun preferWifi() {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val req = NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_WIFI).build()
        cm.requestNetwork(req, object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                cm.bindProcessToNetwork(network)
                main.post { if (showingOffline) loadMachine() }
            }
        })
    }

    private fun startBatteryReports() {
        val report = object : Runnable {
            override fun run() {
                val bm = getSystemService(BatteryManager::class.java)
                val pct = bm?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY) ?: -1
                val charging = bm?.isCharging ?: false
                io.execute { post("api/tablet", "{\"battery\":$pct,\"charging\":$charging}") }
                main.postDelayed(this, 60_000)
            }
        }
        main.post(report)
    }

    private fun post(path: String, json: String): Int = try {
        val c = URL(machineUrl + path).openConnection() as HttpURLConnection
        c.requestMethod = "POST"; c.connectTimeout = 3000; c.readTimeout = 3000; c.doOutput = true
        c.setRequestProperty("Content-Type", "application/json")
        c.outputStream.use { it.write(json.toByteArray()) }
        val code = c.responseCode; c.disconnect(); code
    } catch (_: Exception) { -1 }

    private fun checkPin(pin: String): Boolean = try {
        val c = URL(machineUrl + "api/admin/summary").openConnection() as HttpURLConnection
        c.connectTimeout = 3000; c.readTimeout = 3000
        c.setRequestProperty("X-Admin-Pin", pin)
        val ok = c.responseCode == 200; c.disconnect(); ok
    } catch (_: Exception) { false }

    private fun askServicePin() {
        val input = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_VARIATION_PASSWORD
            hint = "Owner PIN"
        }
        AlertDialog.Builder(this)
            .setTitle("Service mode")
            .setMessage("Enter the owner PIN to leave the kiosk.")
            .setView(input)
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Unlock") { _, _ ->
                val pin = input.text.toString()
                io.execute {
                    val ok = checkPin(pin)
                    main.post {
                        if (ok) {
                            try { stopLockTask() } catch (_: Exception) { }
                            if (dpm.isDeviceOwnerApp(packageName)) dpm.setStatusBarDisabled(admin, false)
                            startActivity(Intent(Settings.ACTION_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                        } else {
                            AlertDialog.Builder(this).setMessage("Wrong PIN, or the machine is not reachable.")
                                .setPositiveButton("OK", null).show()
                        }
                    }
                }
            }
            .show()
    }
}
