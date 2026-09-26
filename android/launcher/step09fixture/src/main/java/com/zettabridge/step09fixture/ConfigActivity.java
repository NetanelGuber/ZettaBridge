package com.zettabridge.step09fixture;

import android.app.Activity;
import android.content.res.Configuration;
import android.os.Bundle;
import android.widget.TextView;

public final class ConfigActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        FixtureLog.guest(this, "config-activity-create");
        TextView view = new TextView(this);
        view.setText("Step 09 configuration fixture");
        setContentView(view);
    }
    @Override public void onConfigurationChanged(Configuration configuration) {
        super.onConfigurationChanged(configuration);
        FixtureLog.event(this, "config-activity-change-" + configuration.orientation);
    }
}
