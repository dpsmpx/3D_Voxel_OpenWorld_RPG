package com.voxelrpg.game;

import android.app.NativeActivity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Активность игры.
 *
 * Java в этом проекте не было вовсе: манифест объявлял
 * android.app.NativeActivity и hasCode="false". Появилась она ради
 * одного — системного выбора файла.
 *
 * Выбор файла нельзя сделать из нативного кода: диалог открывается
 * через startActivityForResult, а ответ приходит в onActivityResult
 * — метод Activity. Перебор файлов по кругу, который стоял вместо
 * него, требовал от игрока класть мир в единственную известную игре
 * папку и жать кнопку, пока не попадётся нужный.
 */
public class MainActivity extends NativeActivity {

    private static final String TAG = "VoxelRPG";
    private static final int REQ_PICK_WORLD = 0x5701;

    /** Куда кладётся выбранный файл: игра читает его уже оттуда. */
    private static final String PICKED_NAME = "picked_world.vxworld";

    /**
     * Выбранный файл готов.
     *
     * @param path полный путь во внутреннем каталоге, или null,
     *             если игрок закрыл диалог, не выбрав ничего.
     */
    private static native void nativeWorldPicked(String path);

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
    }

    /**
     * Открыть системный выбор файла.
     *
     * Зовётся из нативного кода. Через runOnUiThread, потому что
     * игровой цикл крутится в своём потоке, а активность трогать
     * можно только из главного.
     */
    public void openWorldPicker() {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                try {
                    Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                    i.addCategory(Intent.CATEGORY_OPENABLE);
                    // Свой тип файла системе неизвестен, поэтому
                    // показываем всё: иначе список будет пуст.
                    i.setType("*/*");
                    startActivityForResult(i, REQ_PICK_WORLD);
                } catch (Exception e) {
                    Log.w(TAG, "выбор файла не открылся", e);
                    nativeWorldPicked(null);
                }
            }
        });
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != REQ_PICK_WORLD) return;

        Uri uri = (data != null) ? data.getData() : null;
        if (result != RESULT_OK || uri == null) {
            nativeWorldPicked(null);
            return;
        }

        // Копируем СОДЕРЖИМОЕ, а не путь: выбранный документ может
        // лежать в облаке, на карте или в чужом приложении, и
        // открывать его позже по URI уже будет нельзя — разрешение
        // действует до конца этого вызова.
        File out = new File(getFilesDir(), PICKED_NAME);
        InputStream in = null;
        OutputStream os = null;
        try {
            in = getContentResolver().openInputStream(uri);
            if (in == null) { nativeWorldPicked(null); return; }
            os = new FileOutputStream(out);
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
            os.flush();
            nativeWorldPicked(out.getAbsolutePath());
        } catch (Exception e) {
            Log.w(TAG, "выбранный файл не прочитался", e);
            nativeWorldPicked(null);
        } finally {
            try { if (in != null) in.close(); } catch (Exception ignored) {}
            try { if (os != null) os.close(); } catch (Exception ignored) {}
        }
    }
}
