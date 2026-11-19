package org.nimbusvn;

import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;
import java.util.Objects;

/**
 * ONScripter 内核显示面：SDLActivity + 原生库 libonscripter.so。
 *
 * 宿主用 {@link #createIntent} 启动（extra 键见 {@link #EXTRA_ARGV}）。
 * argv 原样交给原生入口 engine/onscripter/src/host/ons_main.cpp：
 *   <游戏目录> [--save-dir <存档目录>] [其它选项]
 *
 * 与另外两个内核不同，ONScripter 不接受"启动文件名"作为首参数：
 * 脚本（nscript.dat / nscript.___）与 *.nsa/*.sar 包由引擎在游戏目录
 * 里自行探测，所以这里只需要目录。
 */
public class ONSActivity extends SDLActivity {
    /** 宿主启动本内核的入口契约：argv 列表。 */
    public static final String EXTRA_ARGV = "org.nimbusvn.ons.ARGV";

    private ArrayList<String> m_gameargs = new ArrayList<>();

    public static Intent createIntent(Context context, ArrayList<String> argv, String saveRoot) {
        Intent intent = new Intent(context, ONSActivity.class);
        intent.putStringArrayListExtra(EXTRA_ARGV, argv);
        // saveRoot 只用于日志/调试；真正的存档目录由宿主拼进 argv（--save-dir）。
        if (saveRoot != null && !saveRoot.isEmpty()) {
            intent.putExtra("ons_save_root", saveRoot);
        }
        return intent;
    }

    // override sdl functions
    static {
        System.loadLibrary("SDL3");
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL3",
            "onscripter"
        };
    }

    @Override
    protected String[] getArguments() {
        return m_gameargs.toArray(new String[0]);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        SDLActivity.nativeSetenv("SDL_ANDROID_ALLOW_RECREATE_ACTIVITY", "1");
        Intent intent = getIntent();
        ArrayList<String> args = intent.getStringArrayListExtra(EXTRA_ARGV);
        if (args != null) m_gameargs = args;
        Log.i("## onscripter", "args=" + m_gameargs);
        super.onCreate(savedInstanceState);
        this.fullscreen();
    }

    @Override
    protected void onResume() {
        super.onResume();
        this.fullscreen();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        if (hasFocus) this.fullscreen();
    }

    /**
     * 只负责 UI 层的两件事：藏 ActionBar、锁横屏。
     * 全屏交给 SDL（SDL_WINDOW_FULLSCREEN 建立窗口后由 SDL 自己维护
     * immersive 状态），这里不要再 setSystemUiVisibility。
     */
    private void fullscreen() {
        try {
            Objects.requireNonNull(this.getSupportActionBar()).hide();
        } catch (NullPointerException ignored) {
        }
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }
}
