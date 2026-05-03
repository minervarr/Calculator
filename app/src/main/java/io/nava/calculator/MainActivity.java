package io.nava.calculator;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;

public class MainActivity extends Activity {
  @Override
  protected void onCreate(Bundle savedInstanceState){
    super.onCreate(savedInstanceState);
    Log.d("Calculator", getHello());
  }
  static {
    System.loadLibrary("calculator");
  }
  public native String getHello();
}

