package android.app;
import android.content.Intent;
import android.content.ContentResolver;
import android.os.Bundle;
import java.io.File;
public class NativeActivity {
    public static final int RESULT_OK = -1;
    protected void onCreate(Bundle b) {}
    protected void onActivityResult(int a, int b, Intent c) {}
    public void startActivityForResult(Intent i, int r) {}
    public void runOnUiThread(Runnable r) {}
    public ContentResolver getContentResolver() { return null; }
    public File getFilesDir() { return null; }
}
