package com.evertopia.app;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public class RomSetupActivity extends Activity {
    private static final String TAG = "EvertopiaSetup";
    private static final int REQ_PICK_ROM = 1001;
    private static final String PREFS = "evertopia_setup";
    private static final String KEY_ASSETS_VERSION = "assets_version_code";

    private TextView statusView;
    private Button pickButton;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // Unpack launcher fonts & images into SDL_AndroidGetInternalStoragePath() (getFilesDir())
        unpackAssetsIfNeeded();

        // Ensure external storage directory exists (SDL_AndroidGetExternalStoragePath())
        File extDir = getExternalFilesDir(null);
        if (extDir != null && !extDir.exists()) {
            extDir.mkdirs();
        }

        // If a ROM is already present, launch the game immediately with no setup prompt
        if (hasRomFile(extDir)) {
            launchGame();
            return;
        }

        buildSetupUi(extDir);
    }

    private boolean hasRomFile(File extDir) {
        if (extDir == null) return false;
        String[] candidates = {
            "evermore.smc", "evermore.sfc",
            "Secret of Evermore.smc", "Secret of Evermore.sfc",
            "rom.cfg"
        };
        for (String name : candidates) {
            File f = new File(extDir, name);
            if (f.exists() && f.length() > 0) {
                return true;
            }
        }
        return false;
    }

    private void buildSetupUi(File extDir) {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundColor(Color.parseColor("#11161F"));
        int pad = dp(32);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("Evertopia — Secret of Evermore");
        title.setTextColor(Color.WHITE);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 26);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.CENTER);
        root.addView(title);

        TextView subtitle = new TextView(this);
        subtitle.setText(
            "Select your legally-owned Secret of Evermore (USA) ROM (.smc or .sfc).\n"
            + "It will be copied into Evertopia's app storage so you only need to pick it once.\n\n"
            + "Gamepad controls: D-Pad / Left Stick + A to select. In-game options menu: L2 or Share."
        );
        subtitle.setTextColor(Color.parseColor("#B8C4D8"));
        subtitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        subtitle.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams subLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        subLp.topMargin = dp(16);
        subLp.bottomMargin = dp(24);
        root.addView(subtitle, subLp);

        LinearLayout btnRow = new LinearLayout(this);
        btnRow.setOrientation(LinearLayout.HORIZONTAL);
        btnRow.setGravity(Gravity.CENTER);

        pickButton = new Button(this);
        pickButton.setText("Select ROM File (.smc / .sfc)");
        pickButton.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        pickButton.setFocusable(true);
        pickButton.setFocusableInTouchMode(true);
        pickButton.setPadding(dp(24), dp(14), dp(24), dp(14));
        pickButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                openRomPicker();
            }
        });
        btnRow.addView(pickButton);

        Button skipButton = new Button(this);
        skipButton.setText("Open Launcher Anyway");
        skipButton.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        skipButton.setFocusable(true);
        skipButton.setPadding(dp(20), dp(14), dp(20), dp(14));
        LinearLayout.LayoutParams skipLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        skipLp.leftMargin = dp(16);
        skipButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                launchGame();
            }
        });
        btnRow.addView(skipButton, skipLp);

        root.addView(btnRow);

        statusView = new TextView(this);
        String pathHint = (extDir != null) ? extDir.getAbsolutePath() : "Android/data/com.evertopia.app/files";
        statusView.setText("App storage folder: " + pathHint);
        statusView.setTextColor(Color.parseColor("#7E8DA6"));
        statusView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        statusView.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams stLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        stLp.topMargin = dp(20);
        root.addView(statusView, stLp);

        setContentView(root);
        pickButton.requestFocus();
    }

    private void openRomPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        try {
            startActivityForResult(intent, REQ_PICK_ROM);
        } catch (Exception e) {
            Toast.makeText(this, "No file picker available: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_PICK_ROM && resultCode == RESULT_OK && data != null) {
            Uri uri = data.getData();
            if (uri != null) {
                importRomUri(uri);
            }
        }
    }

    private void importRomUri(Uri uri) {
        File extDir = getExternalFilesDir(null);
        if (extDir == null) {
            Toast.makeText(this, "External app storage unavailable.", Toast.LENGTH_LONG).show();
            return;
        }
        if (!extDir.exists()) {
            extDir.mkdirs();
        }
        File dest = new File(extDir, "evermore.smc");
        if (statusView != null) {
            statusView.setText("Importing ROM...");
        }
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(dest)) {
            if (in == null) {
                throw new IOException("Could not open selected file");
            }
            byte[] buf = new byte[65536];
            int n;
            long total = 0;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                total += n;
            }
            out.flush();
            if (total < 1024 * 1024) {
                dest.delete();
                Toast.makeText(this, "Selected file is too small to be a Secret of Evermore ROM.", Toast.LENGTH_LONG).show();
                if (statusView != null) {
                    statusView.setText("Invalid ROM size (" + total + " bytes). Please pick evermore.smc / .sfc.");
                }
                return;
            }
            writeRomCfg(extDir, dest);
            Toast.makeText(this, "ROM imported! Opening Evertopia Launcher...", Toast.LENGTH_SHORT).show();
            launchGame();
        } catch (Exception e) {
            Log.e(TAG, "Failed to import ROM", e);
            Toast.makeText(this, "Failed to import ROM: " + e.getMessage(), Toast.LENGTH_LONG).show();
            if (statusView != null) {
                statusView.setText("Import failed: " + e.getMessage());
            }
        }
    }

    private void writeRomCfg(File extDir, File romFile) {
        if (extDir == null || romFile == null) return;
        String content = romFile.getAbsolutePath() + "\n";
        try {
            try (FileOutputStream fos = new FileOutputStream(new File(extDir, "rom.cfg"))) {
                fos.write(content.getBytes("UTF-8"));
            }
            File cfgDir = new File(extDir, "Configs");
            if (!cfgDir.exists()) cfgDir.mkdirs();
            try (FileOutputStream fos = new FileOutputStream(new File(cfgDir, "rom.cfg"))) {
                fos.write(content.getBytes("UTF-8"));
            }
        } catch (IOException e) {
            Log.w(TAG, "Failed to write rom.cfg", e);
        }
    }

    private void launchGame() {
        Intent intent = new Intent(this, EvertopiaActivity.class);
        startActivity(intent);
        finish();
    }

    private void unpackAssetsIfNeeded() {
        int currentVersion = 1;
        try {
            PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
            currentVersion = pi.versionCode;
        } catch (Exception ignored) {
        }

        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        int unpackedVersion = prefs.getInt(KEY_ASSETS_VERSION, -1);
        File checkFont = new File(getFilesDir(), "assets/fonts/LatoLatin-Regular.ttf");
        if (unpackedVersion == currentVersion && checkFont.exists()) {
            return;
        }

        try {
            copyAssetDir("assets", new File(getFilesDir(), "assets"));
            prefs.edit().putInt(KEY_ASSETS_VERSION, currentVersion).apply();
        } catch (IOException e) {
            Log.e(TAG, "Failed to unpack launcher assets", e);
        }
    }

    private void copyAssetDir(String assetPath, File outDir) throws IOException {
        AssetManager am = getAssets();
        String[] entries = am.list(assetPath);
        if (entries == null || entries.length == 0) {
            // Leaf file
            File parent = outDir.getParentFile();
            if (parent != null && !parent.exists()) {
                parent.mkdirs();
            }
            try (InputStream in = am.open(assetPath);
                 OutputStream out = new FileOutputStream(outDir)) {
                byte[] buf = new byte[32768];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                }
            }
            return;
        }
        if (!outDir.exists()) {
            outDir.mkdirs();
        }
        for (String entry : entries) {
            copyAssetDir(assetPath + "/" + entry, new File(outDir, entry));
        }
    }

    private int dp(int value) {
        return Math.round(TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, value, getResources().getDisplayMetrics()));
    }
}
