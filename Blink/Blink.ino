void setup() {
  Serial.begin(115200);
  pinMode(2, OUTPUT); // Built-in LED on most ESP-WROOM-32 dev boards (GPIO2)
  Serial.println("ESP-WROOM-32 Initialized!");
}

void loop() {
  digitalWrite(2, HIGH);
  delay(1000);
  digitalWrite(2, LOW);
  delay(1000);
  Serial.println("Tick");
}
