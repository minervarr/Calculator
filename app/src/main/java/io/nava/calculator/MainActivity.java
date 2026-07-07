// MainActivity.java — NativeActivity subclass adding Storage Access Framework
// (system file picker) export/import for workspace JSON.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// The whole UI is the native Vulkan app; this subclass exists only to host the
// SAF dialogs (which require an Activity + onActivityResult). Native code calls
// exportWorkspace()/importWorkspace() via JNI; imported text is handed back via
// the native method nativeDeliverImport(), which the render loop polls.
package io.nava.calculator;

import android.app.NativeActivity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

public class MainActivity extends NativeActivity {

    // NativeActivity dlopen's the lib through its own loader, which does NOT
    // register it for resolving this class's native methods. Load it here too so
    // nativeDeliverImport() resolves (System.loadLibrary is idempotent).
    static { System.loadLibrary("calculator"); }

    private static final int REQ_EXPORT = 4001;
    private static final int REQ_IMPORT = 4002;

    // Edge-to-edge with the status bar hidden but the NAV BAR kept visible. No
    // IMMERSIVE flag, so the system never auto-hides the nav bar (the previous
    // IMMERSIVE_STICKY made it vanish after a Recents round-trip). LAYOUT_* keeps
    // the window full-size so the native side's nav-bar inset stays correct.
    private void applySystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
              | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
              | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
              | View.SYSTEM_UI_FLAG_FULLSCREEN);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        applySystemUi();
    }

    @Override
    protected void onResume() {
        super.onResume();
        applySystemUi();  // restore after returning from Recents / another app
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) applySystemUi();  // the system clears flags on focus changes
    }

    // Held between the create-document request and its result.
    private String pendingExportJson = null;

    // ── Called from native (JNI) ─────────────────────────────────────────────
    public void exportWorkspace(final String json, final String suggestedName) {
        pendingExportJson = json;
        runOnUiThread(new Runnable() {
            public void run() {
                Intent it = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                it.addCategory(Intent.CATEGORY_OPENABLE);
                it.setType("application/json");
                it.putExtra(Intent.EXTRA_TITLE, suggestedName);
                try {
                    startActivityForResult(it, REQ_EXPORT);
                } catch (Exception e) {
                    pendingExportJson = null;
                }
            }
        });
    }

    public void importWorkspace() {
        runOnUiThread(new Runnable() {
            public void run() {
                Intent it = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                it.addCategory(Intent.CATEGORY_OPENABLE);
                it.setType("*/*");
                try {
                    startActivityForResult(it, REQ_IMPORT);
                } catch (Exception e) {
                    // no picker available; ignore
                }
            }
        });
    }

    // Native handler implemented in saf_bridge.cc (resolved from libcalculator).
    public native void nativeDeliverImport(String json);

    // Standard key-click haptic, called from native on a finger keypress (the
    // native side skips this for stylus input). Respects the user's haptic setting.
    public void hapticTick() {
        final View v = getWindow().getDecorView();
        v.post(new Runnable() {
            public void run() {
                try {
                    v.performHapticFeedback(android.view.HapticFeedbackConstants.KEYBOARD_TAP);
                } catch (Exception e) {
                    // haptics unavailable; ignore
                }
            }
        });
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        Uri uri = (data != null) ? data.getData() : null;
        if (res != RESULT_OK || uri == null) {
            pendingExportJson = null;
            return;
        }
        if (req == REQ_EXPORT) {
            String json = pendingExportJson;
            pendingExportJson = null;
            if (json == null) return;
            try {
                OutputStream os = getContentResolver().openOutputStream(uri, "w");
                if (os != null) {
                    os.write(json.getBytes("UTF-8"));
                    os.flush();
                    os.close();
                }
            } catch (Exception e) {
                // write failed; nothing further to do
            }
        } else if (req == REQ_IMPORT) {
            try {
                InputStream is = getContentResolver().openInputStream(uri);
                ByteArrayOutputStream bos = new ByteArrayOutputStream();
                byte[] buf = new byte[8192];
                int n;
                while ((n = is.read(buf)) > 0) bos.write(buf, 0, n);
                is.close();
                nativeDeliverImport(new String(bos.toByteArray(), "UTF-8"));
            } catch (Exception e) {
                // read failed; ignore
            }
        }
    }
}
