package cn.youlor;
import android.app.ActivityThread;
import java.io.BufferedReader;
import java.io.FileReader;
import java.io.File;

public class Unpacker {
    public static String UNPACK_CONFIG = "/data/local/tmp/unpacker.config";
    public static int UNPACK_INTERVAL = 20 * 1000;
    public static Thread unpackerThread = null;

    public static boolean shouldUnpack() {
        boolean should_unpack = false;
        String processName = ActivityThread.currentProcessName();
        BufferedReader br = null;
        try {
            br = new BufferedReader(new FileReader(UNPACK_CONFIG));
            String line;
            while ((line = br.readLine()) != null) {
                if (line.equals(processName)) {
                    should_unpack = true;
                    break;
                }
            }
            br.close();
        }
        catch (Exception ignored) {
            
        }
        return should_unpack;
    }

    public static void unpack() {
        if (Unpacker.unpackerThread != null) {
            return;
        }
        //开启线程调用
        Unpacker.unpackerThread = new Thread(new Runnable() {
            @Override public void run() {
                while (true) {
                    if (shouldUnpack()) {
                        Unpacker.unpackNative();
                    }
                    try {
                        Thread.sleep(UNPACK_INTERVAL);
                    }
                    catch (InterruptedException e) {
                        e.printStackTrace();
                    }
                }
            }
        });
        Unpacker.unpackerThread.start();     
    }

    public static native void unpackNative();
}
