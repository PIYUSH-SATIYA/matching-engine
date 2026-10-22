## condition variable in first iteratin

- decided to keep the `not-empty` and `not-full` condition variable.
- this will make the behaviour of the ring buffer blocking.
  - producer or consumer will wait till it is not-full or not-empty resp.
