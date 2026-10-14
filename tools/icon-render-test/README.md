# Thumbnail regression

This host test renders the same 42×42 RGB565A8 thumbnail from a file and an
embedded image descriptor at native size. Both frames must match pixel for
pixel with row-at-a-time decoding and no badge-side scaling. It uses the
checked-out LVGL source and firmware decoder default.

```sh
cmake -S tools/icon-render-test -B /tmp/badge-icon-test -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/badge-icon-test -j 8
ctest --test-dir /tmp/badge-icon-test --output-on-failure
```

No badge or ESP-IDF build is needed. The test creates and removes a temporary
`icon-render-test.bin` in its working directory.
