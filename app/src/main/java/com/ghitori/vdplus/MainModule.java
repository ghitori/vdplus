package com.ghitori.vdplus;

import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

import org.json.JSONObject;

import io.github.libxposed.api.XposedModule;
import io.github.libxposed.api.XposedModuleInterface.PackageLoadedParam;

/** 模块入口: 在 VirtualDesktop.Android 进程内加载 native 钩子并下发设置。 */
public class MainModule extends XposedModule {
    private static final String TAG = "VDPlus";
    private static final String TARGET = "VirtualDesktop.Android";
    private static final String LIB = "libvdplus.so";
    private static final String PREFS = "vdplus";

    private static final String K_IMPORTED = "imported_dict";
    private static final String K_USE_IMPORTED = "use_imported_dict";
    private static final String K_NO_MUSIC = "disable_music";
    private static final String K_QUALITY = "disable_quality_sound";
    private static final String K_QUALITY_MASK = "quality_mask";
    private static final String K_EXTEND = "bitrate_extend";
    private static final String K_SCALE = "bitrate_scale";
    private static final String K_UNLOCK = "bitrate_unlock";
    private static final String K_FORCE_LAN = "force_lan";

    private static boolean sLoaded = false;

    public MainModule() {
        super();
    }

    @Override
    public void onPackageLoaded(PackageLoadedParam param) {
        if (!TARGET.equals(param.getPackageName()) || sLoaded) return;
        sLoaded = true;

        String moduleApk = null;
        try {
            ApplicationInfo module = getModuleApplicationInfo();
            if (module != null) moduleApk = module.sourceDir;
        } catch (Throwable t) {
            log(Log.ERROR, TAG, "get module apk failed", t);
        }

        File filesDir = new File(param.getApplicationInfo().dataDir, "files");
        loadNative(moduleApk, filesDir);

        try {
            boolean useImported = readBool(K_USE_IMPORTED, true);
            Map<String, String> dict = new LinkedHashMap<>(loadBuiltinDict(moduleApk));
            if (useImported) dict.putAll(loadImportedDict());
            String[] ks = new String[dict.size()], vs = new String[dict.size()];
            int i = 0;
            for (Map.Entry<String, String> e : dict.entrySet()) { ks[i] = e.getKey(); vs[i] = e.getValue(); i++; }

            nativeSetDict(ks, vs);
            nativeInit();
            nativeSetNoMusic(readBool(K_NO_MUSIC, false));
            nativeSetQuality(readBool(K_QUALITY, false), readInt(K_QUALITY_MASK, 0x7F));
            nativeSetBitrate(readBool(K_EXTEND, false), readBool(K_UNLOCK, false), readFloat(K_SCALE, 2.0f));
            nativeSetForceLan(readBool(K_FORCE_LAN, true));
            log(Log.INFO, TAG, "dict=" + dict.size() + " imported=" + useImported);
        } catch (Throwable t) {
            log(Log.ERROR, TAG, "init failed", t);
        }
    }

    private void loadNative(String moduleApk, File filesDir) {
        try {
            if (moduleApk == null) throw new IllegalStateException("no module apk");
            ZipFile zf = new ZipFile(moduleApk);
            try {
                ZipEntry e = zf.getEntry("lib/arm64-v8a/" + LIB);
                if (e == null) throw new IllegalStateException("no " + LIB);
                filesDir.mkdirs();
                File out = new File(filesDir, LIB);
                InputStream in = zf.getInputStream(e);
                FileOutputStream fos = new FileOutputStream(out);
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) fos.write(buf, 0, n);
                fos.close();
                in.close();
                System.load(out.getAbsolutePath());
                log(Log.INFO, TAG, "native loaded");
            } finally {
                zf.close();
            }
        } catch (Throwable t) {
            log(Log.ERROR, TAG, "native load failed", t);
        }
    }

    private Map<String, String> loadBuiltinDict(String moduleApk) {
        Map<String, String> m = new LinkedHashMap<>();
        if (moduleApk == null) return m;
        try (ZipFile zf = new ZipFile(moduleApk)) {
            ZipEntry e = zf.getEntry("assets/zh_ui_dict.json");
            if (e != null) {
                try (InputStream in = zf.getInputStream(e)) {
                    parseDict(new String(readAll(in), StandardCharsets.UTF_8), m);
                }
            }
        } catch (Throwable t) {
            log(Log.WARN, TAG, "builtin dict failed", t);
        }
        return m;
    }

    private Map<String, String> loadImportedDict() {
        Map<String, String> m = new LinkedHashMap<>();
        try {
            String json = getRemotePreferences(PREFS).getString(K_IMPORTED, null);
            if (json != null) parseDict(json, m);
        } catch (Throwable t) {
            log(Log.WARN, TAG, "imported dict failed", t);
        }
        return m;
    }

    private boolean readBool(String key, boolean def) {
        try { return getRemotePreferences(PREFS).getBoolean(key, def); } catch (Throwable t) { return def; }
    }

    private int readInt(String key, int def) {
        try { return getRemotePreferences(PREFS).getInt(key, def); } catch (Throwable t) { return def; }
    }

    private float readFloat(String key, float def) {
        try { return getRemotePreferences(PREFS).getFloat(key, def); } catch (Throwable t) { return def; }
    }

    private static void parseDict(String json, Map<String, String> out) {
        try {
            JSONObject o = new JSONObject(json);
            for (Iterator<String> it = o.keys(); it.hasNext(); ) {
                String k = it.next();
                out.put(k, o.optString(k, ""));
            }
        } catch (Throwable ignored) {
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
        return b.toByteArray();
    }


    public native void nativeInit();
    public native void nativeSetDict(String[] keys, String[] vals);
    public native void nativeSetNoMusic(boolean disable);
    public native void nativeSetQuality(boolean enable, int mask);
    public native void nativeSetBitrate(boolean extend, boolean unlock, float scale);
    public native void nativeSetForceLan(boolean enable);
}
