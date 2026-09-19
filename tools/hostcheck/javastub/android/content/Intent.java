package android.content;
import android.net.Uri;
public class Intent {
    public static final String ACTION_OPEN_DOCUMENT = "";
    public static final String CATEGORY_OPENABLE = "";
    public Intent(String action) {}
    public Intent addCategory(String c) { return this; }
    public Intent setType(String t) { return this; }
    public Uri getData() { return null; }
}
