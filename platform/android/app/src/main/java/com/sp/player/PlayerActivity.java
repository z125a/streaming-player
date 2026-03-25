package com.sp.player;

import android.app.Activity;
import android.os.Bundle;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;

/**
 * Simple player activity with SurfaceView for video rendering.
 */
public class PlayerActivity extends Activity implements SurfaceHolder.Callback {

    private NativePlayer player;
    private SurfaceView surfaceView;
    private Button btnPlayPause;
    private TextView tvStatus;
    private String url;
    private boolean isPlaying = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_player);

        surfaceView = findViewById(R.id.surface_view);
        btnPlayPause = findViewById(R.id.btn_play_pause);
        tvStatus = findViewById(R.id.tv_status);

        surfaceView.getHolder().addCallback(this);

        url = getIntent().getStringExtra("url");
        if (url == null) url = "http://127.0.0.1:8080/live/stream.m3u8";

        player = new NativePlayer();

        btnPlayPause.setOnClickListener(v -> togglePlayPause());
    }

    private void togglePlayPause() {
        if (!isPlaying) {
            if (player.open(url)) {
                player.setSurface(surfaceView.getHolder().getSurface());
                player.play();
                isPlaying = true;
                btnPlayPause.setText("Stop");
                tvStatus.setText(player.isLive() ? "LIVE" : "Playing");
            } else {
                tvStatus.setText("Failed to open");
            }
        } else {
            player.stop();
            isPlaying = false;
            btnPlayPause.setText("Play");
            tvStatus.setText("Stopped");
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (player != null && isPlaying) {
            player.setSurface(holder.getSurface());
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        if (player != null) {
            player.setSurface(null);
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (player != null) {
            player.stop();
            player.release();
        }
    }
}
