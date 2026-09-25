#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <RTClib.h>
#define PIN_MQ3          A0
#define PIN_DHT           2
#define PIN_RELAY_AUTO    3   
#define PIN_RELAY_VOLANTE 4  
#define PIN_BUZZER        5
#define PIN_LED_VERDE     6
#define PIN_LED_ROJO      7    
#define RELE_ON   LOW
#define RELE_OFF  HIGH
#define UMBRAL_PRESENCIA     10   
#define UMBRAL_ALCOHOL       45  
#define INTENTOS_MAX          3
#define TIEMPO_BLOQUEO    30000
#define TIEMPO_PRECALENTADO 60000
#define DURACION_SOPLIDO     5000
#define INTERVALO_RETEST     100000  
#define CMD_SOMNOLENCIA        'S'   
#define COOLDOWN_SOMNOLENCIA  8000   

LiquidCrystal_I2C lcd(0x27, 16, 2);
DHT dht(PIN_DHT, DHT11);
RTC_DS3231 rtc;

int baseMQ3 = 0;

int  intentosFallidos   = 0;
bool bloqueado          = false;
bool arranqueHabilitado = false;
unsigned long tiempoBloqueo     = 0;
unsigned long ultimoRetest      = 0;
unsigned long intervaloRetest   = 0;
volatile bool somnolenciaPendiente = false;
unsigned long ultimaAlertaSomnolencia = 0;

void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(A1));

  pinMode(PIN_RELAY_AUTO,    OUTPUT);
  pinMode(PIN_RELAY_VOLANTE, OUTPUT);
  pinMode(PIN_BUZZER,        OUTPUT);
  pinMode(PIN_LED_VERDE,     OUTPUT);
  pinMode(PIN_LED_ROJO,      OUTPUT);

  digitalWrite(PIN_RELAY_AUTO,    RELE_OFF);  
  digitalWrite(PIN_RELAY_VOLANTE, RELE_OFF);
  digitalWrite(PIN_LED_VERDE,     LOW);
  digitalWrite(PIN_LED_ROJO,      LOW);
  digitalWrite(PIN_BUZZER,        LOW);

  Wire.begin();
  lcd.init();
  lcd.backlight();
  dht.begin();

  if (!rtc.begin()) {
    digitalWrite(PIN_LED_ROJO, HIGH);  
    mostrarMensaje("Error RTC", "Revisar cables");
    while (1);
  }
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  unsigned long inicio = millis();
  while (millis() - inicio < TIEMPO_PRECALENTADO) {
    int restante = (TIEMPO_PRECALENTADO - (millis() - inicio)) / 1000;
    lcd.setCursor(0, 0); lcd.print("Precalentando   ");
    lcd.setCursor(0, 1);
    lcd.print("MQ3... "); lcd.print(restante); lcd.print("s   ");
    delay(500);
  }

  mostrarMensaje("Calibrando...", "No soples aun");
  long suma = 0;
  for (int i = 0; i < 20; i++) {
    suma += analogRead(PIN_MQ3);
    delay(100);
  }
  baseMQ3 = suma / 20;
  Serial.print("Base MQ3: "); Serial.println(baseMQ3);

  intervaloRetest = INTERVALO_RETEST;

  vaciarSerial();

  mostrarMensaje("SAFEDRIVE v1.1", "Listo!");
  delay(1500);
  mostrarMensaje("Sople el sensor", "cuando indique");
}

void loop() {
  revisarSerial();

  if (somnolenciaPendiente) {
    somnolenciaPendiente = false;
    if (millis() - ultimaAlertaSomnolencia > COOLDOWN_SOMNOLENCIA ||
        ultimaAlertaSomnolencia == 0) {
      ultimaAlertaSomnolencia = millis();
      activarAlertaSomnolencia();
      vaciarSerial();
    }
    return;
  }

  if (bloqueado) {
    long restante = (TIEMPO_BLOQUEO - (millis() - tiempoBloqueo)) / 1000;
    if (restante <= 0) {
      bloqueado         = false;
      intentosFallidos  = 0;
      arranqueHabilitado = false;
      digitalWrite(PIN_LED_ROJO, LOW); 
      mostrarMensaje("Sistema listo", "Intente de nuevo");
      esperar(1500);
      mostrarMensaje("Sople el sensor", "cuando indique");
    } else {
      lcd.setCursor(0, 0); lcd.print("BLOQUEADO       ");
      lcd.setCursor(0, 1);
      lcd.print("Espere: "); lcd.print(restante); lcd.print("s   ");
      esperar(500);
    }
    return;
  }

  if (arranqueHabilitado && millis() - ultimoRetest > intervaloRetest) {
    reTestEnViaje();
    return;
  }

  if (!arranqueHabilitado) {
    mostrarMensaje("Listo!", "Sople ahora...");
    esperar(1000);
    int picoDiff = capturarSoplido(DURACION_SOPLIDO);
    evaluarResultado(picoDiff);
    esperar(1500);
  }
}

void revisarSerial() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == CMD_SOMNOLENCIA) {
      somnolenciaPendiente = true;
    }
  }
}

void vaciarSerial() {
  while (Serial.available() > 0) {
    Serial.read();
  }
}
void esperar(unsigned long ms) {
  unsigned long inicio = millis();
  while (millis() - inicio < ms) {
    revisarSerial();
    delay(5);
  }
}

int capturarSoplido(unsigned long duracion) {
  unsigned long inicio = millis();
  int pico = 0;
  while (millis() - inicio < duracion) {
    int crudo = analogRead(PIN_MQ3);
    int diff = crudo - baseMQ3;
    if (diff < 0) diff = 0;
    if (diff > pico) pico = diff;

    int restante = (duracion - (millis() - inicio)) / 1000;
    lcd.setCursor(0, 0); lcd.print("Analizando...  ");
    lcd.setCursor(0, 1);
    lcd.print("Tiempo: "); lcd.print(restante + 1); lcd.print("s   ");
    esperar(100);
  }

  float humedad  = dht.readHumidity();
  float tempAire = dht.readTemperature();
  Serial.print("Pico diff: "); Serial.print(pico);
  Serial.print(" | Humedad: "); Serial.print(humedad);
  Serial.print("% | Temp: "); Serial.println(tempAire);

  return pico;
}

void evaluarResultado(int picoDiff) {
  if (picoDiff < UMBRAL_PRESENCIA) {
    mostrarMensaje("No se detecto", "soplido. Reintente");
    beepCorto();
    esperar(2000);
    mostrarMensaje("Sople el sensor", "cuando indique");
    return;
  }

  if (picoDiff < UMBRAL_ALCOHOL) {
    habilitarArranque(picoDiff);
  } else {
    manejarPositivo(picoDiff);
  }
}

void habilitarArranque(int nivel) {
  intentosFallidos   = 0;
  arranqueHabilitado = true;

  digitalWrite(PIN_RELAY_AUTO,    RELE_ON);   
  digitalWrite(PIN_RELAY_VOLANTE, RELE_ON);    
  digitalWrite(PIN_LED_VERDE,     HIGH);
  digitalWrite(PIN_LED_ROJO,      LOW);  
  digitalWrite(PIN_BUZZER,        LOW);

  registrarEvento("NEGATIVO - ARRANQUE OK", nivel);

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("NEGATIVO  OK    ");
  lcd.setCursor(0, 1);
  lcd.print("Nivel: "); lcd.print(nivel);

  Serial.println("NEGATIVO_OK");

  ultimoRetest    = millis();
  intervaloRetest = INTERVALO_RETEST;
}

void manejarPositivo(int nivel) {
  intentosFallidos++;
  arranqueHabilitado = false;

  digitalWrite(PIN_RELAY_AUTO,    RELE_OFF);   
  digitalWrite(PIN_RELAY_VOLANTE, RELE_OFF);   
  digitalWrite(PIN_LED_VERDE,     LOW);
  digitalWrite(PIN_LED_ROJO,      HIGH); 

  registrarEvento("POSITIVO - BLOQUEADO", nivel);
  Serial.println("ALCOHOL_POSITIVO");

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("POSITIVO        ");
  lcd.setCursor(0, 1);
  lcd.print("Niv:"); lcd.print(nivel);
  lcd.print(" Int:"); lcd.print(intentosFallidos);

  beepAlarma();
  esperar(3000);

  if (intentosFallidos >= INTENTOS_MAX) {
    activarBloqueoExtendido(nivel);
  } else {
    mostrarMensaje("BLOQUEADO", "Reintente");
    esperar(1500);
    mostrarMensaje("Sople el sensor", "cuande indique");
  }
}

void activarBloqueoExtendido(int nivel) {
  bloqueado    = true;
  tiempoBloqueo = millis();

  digitalWrite(PIN_RELAY_AUTO,    RELE_OFF);  
  digitalWrite(PIN_RELAY_VOLANTE, RELE_OFF);  
  digitalWrite(PIN_LED_ROJO,      HIGH);

  registrarEvento("BLOQUEO EXTENDIDO - 3 INTENTOS", nivel);
  Serial.println("BLOQUEO_EXTENDIDO");

  digitalWrite(PIN_BUZZER, HIGH);
  delay(2000);
  digitalWrite(PIN_BUZZER, LOW);
}

void reTestEnViaje() {
  mostrarMensaje("RE-TEST", "Sople ahora!");
  beepCorto();
  esperar(1000);

  int picoDiff = capturarSoplido(DURACION_SOPLIDO);

  if (picoDiff < UMBRAL_PRESENCIA) {
    registrarEvento("RETEST SIN RESPUESTA", 0);
    Serial.println("RETEST_SIN_RESPUESTA");
    activarAlertaSomnolencia();
    return;
  }

  if (picoDiff < UMBRAL_ALCOHOL) {
    registrarEvento("RETEST OK", picoDiff);
    Serial.println("RETEST_OK");
    mostrarMensaje("Retest OK", "Continue viaje");
    ultimoRetest    = millis();
    intervaloRetest = INTERVALO_RETEST;
    esperar(2000);
    mostrarMensaje("SAFEDRIVE", "Activo");
  } else {
    registrarEvento("RETEST POSITIVO", picoDiff);
    Serial.println("RETEST_POSITIVO");
    manejarPositivo(picoDiff);
  }
}

void activarAlertaSomnolencia() {
  registrarEvento("SIN RESPUESTA AL RETEST", 0);
  Serial.println("SOMNOLENCIA");

  bool rojoPrevio = digitalRead(PIN_LED_ROJO);

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("!ATENCION!      ");
  lcd.setCursor(0, 1); lcd.print("Detengase seguro");

  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_BUZZER,   HIGH);
    digitalWrite(PIN_LED_ROJO, HIGH);  
    delay(500);
    digitalWrite(PIN_BUZZER,   LOW);
    digitalWrite(PIN_LED_ROJO, LOW);
    delay(300);
  }

  for (int i = 0; i < 5; i++) {
    digitalWrite(PIN_RELAY_VOLANTE, RELE_ON);  delay(200);  
    digitalWrite(PIN_RELAY_VOLANTE, RELE_OFF); delay(200);   
  }

  digitalWrite(PIN_LED_ROJO, rojoPrevio); 

  delay(3000);
  mostrarMensaje("SAFEDRIVE", "Activo");
}

void registrarEvento(const char* tipo, int nivel) {
  DateTime ahora = rtc.now();

  Serial.print("[");
  Serial.print(ahora.day());    Serial.print("/");
  Serial.print(ahora.month());  Serial.print("/");
  Serial.print(ahora.year());   Serial.print(" ");
  Serial.print(ahora.hour());   Serial.print(":");
  if (ahora.minute() < 10) Serial.print("0");
  Serial.print(ahora.minute()); Serial.print(":");
  if (ahora.second() < 10) Serial.print("0");
  Serial.print(ahora.second());
  Serial.print("] ");
  Serial.print(tipo);
  Serial.print(" | Diferencia MQ3: ");
  Serial.println(nivel);
}

void mostrarMensaje(const char* l1, const char* l2) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(l1);
  lcd.setCursor(0, 1); lcd.print(l2);
}

void beepCorto() {
  digitalWrite(PIN_BUZZER, HIGH); delay(300);
  digitalWrite(PIN_BUZZER, LOW);
}

void beepAlarma() {
  for (int i = 0; i < 5; i++) {
    digitalWrite(PIN_BUZZER, HIGH); delay(200);
    digitalWrite(PIN_BUZZER, LOW);  delay(200);
  }
}