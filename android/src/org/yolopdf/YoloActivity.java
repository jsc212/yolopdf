// SPDX-License-Identifier: AGPL-3.0-only
package org.yolopdf;

import android.content.Intent;
import android.os.Bundle;
import org.qtproject.qt.android.bindings.QtActivity;

public class YoloActivity extends QtActivity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        MobileIntegration.receiveIntent(getIntent());
    }
    @Override public void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        MobileIntegration.receiveIntent(intent);
    }
}
