package com.zettabridge.step04;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

public final class ProbeActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this);
        text.setText(ProbeRunner.run(this, "main"));
        setContentView(text);
    }
}
