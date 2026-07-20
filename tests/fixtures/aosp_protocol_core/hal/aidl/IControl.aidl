package android.hardware.foo;

import android.hardware.foo.ICallback;

interface IControl {
    oneway void subscribe(in ICallback callback);
}
