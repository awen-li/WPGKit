void target() {}

void relay(void (*callback)()) {
  callback();
}

void entry() {
  relay(target);
}

int main() {
  entry();
}
