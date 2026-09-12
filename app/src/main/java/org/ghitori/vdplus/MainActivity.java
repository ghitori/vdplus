package org.ghitori.vdplus;

import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;

import com.google.android.material.appbar.MaterialToolbar;
import com.google.android.material.bottomnavigation.BottomNavigationView;
import com.google.android.material.card.MaterialCardView;
import com.google.android.material.color.MaterialColors;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.google.android.material.materialswitch.MaterialSwitch;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Iterator;
import java.util.Locale;

import org.json.JSONObject;

import io.github.libxposed.service.XposedService;
import io.github.libxposed.service.XposedServiceHelper;

public class MainActivity extends AppCompatActivity {
    private static final int REQ_IMPORT = 1001;
    private static final String PREFS = "vdplus";
    private static final String VD_PKG = "VirtualDesktop.Android";

    private static final String K_IMPORTED = "imported_dict";
    private static final String K_TIME = "imported_time";
    private static final String K_USE_IMPORTED = "use_imported_dict";
    private static final String K_NO_MUSIC = "disable_music";
    private static final String K_QUALITY = "disable_quality_sound";
    private static final String K_QUALITY_MASK = "quality_mask";
    private static final String K_EXTEND = "bitrate_extend";
    private static final String K_SCALE = "bitrate_scale";
    private static final String K_UNLOCK = "bitrate_unlock";
    private static final String K_FORCE_LAN = "force_lan";

    // listener 为全局静态, 必须用单例, 否则 Activity 重建后回调会送到旧实例
    private static volatile XposedService sService = null;
    private static MainActivity sCurrent = null;
    private static boolean sRegistered = false;
    private static final XposedServiceHelper.OnServiceListener sListener = new XposedServiceHelper.OnServiceListener() {
        @Override public void onServiceBind(XposedService service) { sService = service; notifyServiceChanged(); }
        @Override public void onServiceDied(XposedService service) { sService = null; notifyServiceChanged(); }
    };

    private boolean binding = false;

    private MaterialToolbar toolbar;
    private BottomNavigationView bottomNav;
    private View pageHome, pageOptions;
    private MaterialCardView homeStatus;
    private ImageView homeStatusIcon;
    private TextView homeStatusText, homeStatusSub;
    private View rowImport, rowRestore;
    private LinearLayout homeInfoSystem, homeInfoDict;
    private MaterialSwitch swUseDict, swNoMusic, swNoPerf, swExtend, swUnlock, swForceLan;

    private static void ensureServiceRegistered() {
        if (sRegistered) return;
        sRegistered = true;
        try { XposedServiceHelper.registerListener(sListener); } catch (Throwable t) { sRegistered = false; }
    }

    private static void notifyServiceChanged() {
        MainActivity a = sCurrent;
        if (a != null) a.runOnUiThread(a::refresh);
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        setContentView(R.layout.activity_main);

        toolbar = findViewById(R.id.toolbar);
        bottomNav = findViewById(R.id.bottom_nav);
        pageHome = findViewById(R.id.page_home);
        pageOptions = findViewById(R.id.page_options);
        homeStatus = findViewById(R.id.home_status);
        homeStatusIcon = findViewById(R.id.home_status_icon);
        homeStatusText = findViewById(R.id.home_status_text);
        homeStatusSub = findViewById(R.id.home_status_sub);
        homeInfoSystem = findViewById(R.id.home_info_system);
        homeInfoDict = findViewById(R.id.home_info_dict);
        rowImport = findViewById(R.id.row_import);
        rowRestore = findViewById(R.id.row_restore);
        swUseDict = findViewById(R.id.sw_use_dict);
        swNoMusic = findViewById(R.id.sw_no_music);
        swNoPerf = findViewById(R.id.sw_no_perf);
        swExtend = findViewById(R.id.sw_extend);
        swUnlock = findViewById(R.id.sw_unlock);
        swForceLan = findViewById(R.id.sw_force_lan);

        bottomNav.setOnItemSelectedListener(item -> {
            showPage(item.getItemId() == R.id.nav_options ? 1 : 0);
            return true;
        });

        swUseDict.setOnCheckedChangeListener((v, c) -> save(K_USE_IMPORTED, c));
        swNoMusic.setOnCheckedChangeListener((v, c) -> save(K_NO_MUSIC, c));
        swNoPerf.setOnCheckedChangeListener((v, c) -> save(K_QUALITY, c));
        swExtend.setOnCheckedChangeListener((v, c) -> save(K_EXTEND, c));
        swUnlock.setOnCheckedChangeListener((v, c) -> save(K_UNLOCK, c));
        swForceLan.setOnCheckedChangeListener((v, c) -> save(K_FORCE_LAN, c));
        swNoPerf.setOnLongClickListener(v -> { showSoundDialog(); return true; });
        swExtend.setOnLongClickListener(v -> { showScaleDialog(); return true; });
        findViewById(R.id.row_no_perf).setOnLongClickListener(v -> { showSoundDialog(); return true; });
        findViewById(R.id.row_extend).setOnLongClickListener(v -> { showScaleDialog(); return true; });

        rowImport.setOnClickListener(v -> startImport());
        rowRestore.setOnClickListener(v -> restoreDict());

        ensureServiceRegistered();
        showPage(0);
        refresh();
    }

    @Override protected void onResume() {
        super.onResume();
        sCurrent = this;
        if (sService == null) { try { XposedServiceHelper.registerListener(sListener); } catch (Throwable ignored) {} }
        ensureServiceRegistered();
        refresh();
    }

    @Override protected void onPause() {
        if (sCurrent == this) sCurrent = null;
        super.onPause();
    }

    private void showPage(int p) {
        pageHome.setVisibility(p == 0 ? View.VISIBLE : View.GONE);
        pageOptions.setVisibility(p == 1 ? View.VISIBLE : View.GONE);
        toolbar.setTitle(p == 0 ? getString(R.string.app_name) : "选项");
        if (p == 0) refresh();
    }

    private void refresh() {
        boolean active = sService != null;

        int container = MaterialColors.getColor(this,
                active ? com.google.android.material.R.attr.colorPrimaryContainer
                       : com.google.android.material.R.attr.colorErrorContainer,
                active ? Color.parseColor("#EADDFF") : Color.parseColor("#F9DEDC"));
        int on = MaterialColors.getColor(this,
                active ? com.google.android.material.R.attr.colorOnPrimaryContainer
                       : com.google.android.material.R.attr.colorOnErrorContainer,
                active ? Color.parseColor("#21005D") : Color.parseColor("#410E0B"));

        homeStatus.setCardBackgroundColor(container);
        homeStatusIcon.setImageResource(active ? R.drawable.ic_check_circle : R.drawable.ic_warning);
        homeStatusIcon.setColorFilter(on);
        homeStatusText.setText(active ? "已激活" : "未激活");
        homeStatusText.setTextColor(on);
        homeStatusSub.setTextColor(on);
        homeStatusSub.setText(active ? "" : "请在 LSPosed 中启用模块，并重启 Virtual Desktop");
        homeStatusSub.setVisibility(active ? View.GONE : View.VISIBLE);

        SharedPreferences p = getSharedPreferences(PREFS, MODE_PRIVATE);
        int imported = Math.max(countDict(p.getString(K_IMPORTED, null)), 0);
        String importedTime = p.getString(K_TIME, null);
        boolean useImported = p.getBoolean(K_USE_IMPORTED, true);

        homeInfoSystem.removeAllViews();
        addInfo(homeInfoSystem, R.drawable.ic_module, "模块版本", version(getPackageName()));
        addInfo(homeInfoSystem, R.drawable.ic_app, "Virtual Desktop 版本", version(VD_PKG));

        homeInfoDict.removeAllViews();
        addInfo(homeInfoDict, R.drawable.ic_dict, "内置词典", bundledCount() + " 条");
        addInfo(homeInfoDict, R.drawable.ic_dict, "导入词典",
                imported > 0 ? (imported + " 条" + (importedTime != null ? " · " + importedTime : "")) : "无");
        addInfo(homeInfoDict, R.drawable.ic_check_circle, "当前生效",
                useImported && imported > 0 ? "导入词典" : "内置词典");

        binding = true;
        swUseDict.setChecked(p.getBoolean(K_USE_IMPORTED, true));
        swNoMusic.setChecked(p.getBoolean(K_NO_MUSIC, false));
        swNoPerf.setChecked(p.getBoolean(K_QUALITY, false));
        swExtend.setChecked(p.getBoolean(K_EXTEND, false));
        swUnlock.setChecked(p.getBoolean(K_UNLOCK, false));
        swForceLan.setChecked(p.getBoolean(K_FORCE_LAN, true));
        binding = false;
    }

    private void addInfo(LinearLayout box, int iconRes, String label, String value) {
        View row = LayoutInflater.from(this).inflate(R.layout.item_info, box, false);
        ImageView icon = row.findViewById(R.id.info_icon);
        icon.setImageResource(iconRes);
        icon.setColorFilter(MaterialColors.getColor(this,
                com.google.android.material.R.attr.colorPrimary, Color.parseColor("#6750A4")));
        ((TextView) row.findViewById(R.id.info_label)).setText(label);
        ((TextView) row.findViewById(R.id.info_value)).setText(value);
        box.addView(row);
    }

    private void save(String key, Object value) {
        if (binding) return;
        put(getSharedPreferences(PREFS, MODE_PRIVATE).edit(), key, value).apply();
        if (sService != null) {
            try { put(sService.getRemotePreferences(PREFS).edit(), key, value).commit(); } catch (Throwable ignored) {}
        }
    }

    private static SharedPreferences.Editor put(SharedPreferences.Editor e, String key, Object v) {
        if (v instanceof Boolean) e.putBoolean(key, (Boolean) v);
        else if (v instanceof Integer) e.putInt(key, (Integer) v);
        else if (v instanceof Float) e.putFloat(key, (Float) v);
        else if (v instanceof Long) e.putLong(key, (Long) v);
        else e.putString(key, String.valueOf(v));
        return e;
    }

    // ---------- 词典 ----------
    private void startImport() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/json", "text/plain", "text/json"});
        startActivityForResult(i, REQ_IMPORT);
    }

    private void restoreDict() {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().remove(K_IMPORTED).remove(K_TIME).apply();
        if (sService != null) {
            try { sService.getRemotePreferences(PREFS).edit().remove(K_IMPORTED).remove(K_TIME).commit(); } catch (Throwable ignored) {}
        }
        save(K_USE_IMPORTED, false);
        refresh();
        Toast.makeText(this, "已恢复默认词典", Toast.LENGTH_SHORT).show();
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != REQ_IMPORT || res != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (uri == null) return;
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            if (in == null) throw new IllegalStateException("无法读取文件");
            String json = new String(readAll(in), StandardCharsets.UTF_8);
            int count = countDict(json);
            if (count <= 0) throw new IllegalStateException("JSON 格式无效");
            String time = new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.getDefault()).format(new Date());
            save(K_IMPORTED, json);
            save(K_TIME, time);
            save(K_USE_IMPORTED, true);
            refresh();
            Toast.makeText(this, "已导入 " + count + " 条", Toast.LENGTH_SHORT).show();
        } catch (Throwable t) {
            Toast.makeText(this, "导入失败: " + t.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    // ---------- 悬浮菜单 ----------
    private void showSoundDialog() {
        View v = LayoutInflater.from(this).inflate(R.layout.dialog_sounds, null);
        int[] ids = {R.id.snd0, R.id.snd1, R.id.snd2, R.id.snd3, R.id.snd4, R.id.snd5, R.id.snd6};
        MaterialSwitch[] sw = new MaterialSwitch[ids.length];
        int mask = getSharedPreferences(PREFS, MODE_PRIVATE).getInt(K_QUALITY_MASK, 0x7F);
        for (int i = 0; i < ids.length; i++) {
            sw[i] = v.findViewById(ids[i]);
            sw[i].setChecked((mask & (1 << i)) != 0);
        }
        new MaterialAlertDialogBuilder(this)
                .setTitle("选择要静音的提示音")
                .setView(v)
                .setPositiveButton("完成", null)
                .setOnDismissListener(dd -> {
                    int m = 0;
                    for (int i = 0; i < sw.length; i++) if (sw[i].isChecked()) m |= (1 << i);
                    save(K_QUALITY_MASK, m);
                })
                .show();
    }

    private void showScaleDialog() {
        View v = LayoutInflater.from(this).inflate(R.layout.dialog_scale, null);
        EditText in = v.findViewById(R.id.scale_input);
        in.setText(formatScale(getSharedPreferences(PREFS, MODE_PRIVATE).getFloat(K_SCALE, 2.0f)));
        new MaterialAlertDialogBuilder(this)
                .setTitle("码率倍率")
                .setView(v)
                .setPositiveButton("完成", null)
                .setOnDismissListener(dd -> {
                    float s;
                    try { s = Float.parseFloat(in.getText().toString().trim()); } catch (Throwable t) { s = 2.0f; }
                    if (s < 1f) s = 1f;
                    if (s > 3f) s = 3f;
                    save(K_SCALE, s);
                })
                .show();
    }

    // ---------- 工具 ----------
    private String version(String pkg) {
        try { return getPackageManager().getPackageInfo(pkg, 0).versionName; }
        catch (Throwable t) { return VD_PKG.equals(pkg) ? "未安装" : "—"; }
    }

    private int bundledCount() {
        try (InputStream in = getAssets().open("zh_ui_dict.json")) {
            return countDict(new String(readAll(in), StandardCharsets.UTF_8));
        } catch (Throwable t) { return 0; }
    }

    private static int countDict(String json) {
        if (json == null) return 0;
        try {
            JSONObject o = new JSONObject(json);
            int c = 0;
            for (Iterator<String> it = o.keys(); it.hasNext(); ) { it.next(); c++; }
            return c;
        } catch (Throwable t) { return -1; }
    }

    private static String formatScale(float s) {
        if (s == Math.round(s)) return String.valueOf((int) s);
        return String.format(Locale.getDefault(), "%.2f", s);
    }

    private static byte[] readAll(InputStream in) throws java.io.IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
        return b.toByteArray();
    }
}
