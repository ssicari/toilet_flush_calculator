#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPIFFS.h>

#ifndef WIFI_SSID
#define WIFI_SSID "WiFi-SSID"    // SSID (default placeholder)
#endif

#ifndef WIFI_PASS
#define WIFI_PASS "WiFi-PASS"    // WiFi Password (default placeholder)
#endif
 
 // Define Pins
const int water_sensor_pin = A0;

// Create the lcd object
LiquidCrystal_I2C lcd(0x27, 16,2); 

static const double water_per_flush = 1.3; // My toilet claims 1.0 - 1.6 gpf for an average of 1.3 gpf
static double total_price = 0.00;
static int num_of_daily_flushes = 0;

// Reset Flag for ISR, some needed functions are not fast enough for the actual service routine so need to move it
// static bool reset_flag = false;

// Most recent price per flush
static double most_recent_price = 0;

// Set up date and time config
static const char* ntp_server = "pool.ntp.org";
static const long  gmt_offset_sec = -18000;
static const int   daylight_offset_sec = 3600;
static int setup_day = 0;
static int setup_month = 0;
static int setup_year = 0;
static int time_counter = 0;

// Configure ntfy topic
static const String ntfy_topic = "toilet_flush_calculator_uuid";

// Persitent file path
static const String file_path = "/persistent_data";

void connectToWIFI() {
  // connect to wifi
  // pass ssid and pass through platform.ino
  // WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  printf("Attempting to Connect to WiFi network");

  int attempts = 0;
  while(WiFi.status() != WL_CONNECTED) {
    delay(500);
    printf(".");
    attempts++;
    if (attempts >= 20) {  // ~10 seconds timeout
      printf("\nFailed to Connect to WiFi\n");
      break;
    }
  }
  if(WiFi.status() == WL_CONNECTED) {
    printf("\nSuccessfully Connected to WiFi\n");
  }
}

void disconnectFromWIFI() {
  // after check disconnect from wifi so it is not at all on the network when not needed
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  if(WiFi.status() == WL_NO_SHIELD) {
    printf("\nSuccessfully Disconnected to WiFi\n");
  }
}

String extractValue(String html) {
  int detected = 0;
  int captured = 0;
  int start = 0;
  while(detected < 12)
  {
    captured = html.indexOf("Lowell", start);
    detected++;
    start = captured + 6;
  }
  html = html.substring(start, (start + 1000));
  // Serial.println(html);
  detected = 0;
  start = 0;
  while(detected < 2)
  {
    captured = html.indexOf("$$", start);
    detected++;
    start = captured + 2;
  }
  html = html.substring((start), (start + 5));
  return html;
}

double findPricePerFlush() {
  // keep this static for now until I figure out how to parse the html from this url
  String server_url = "https:domain.com";
  HTTPClient http;
  String payload;
  String extracted_water_value = "";
  double price_per_5000_gallons = 0;
  double price_per_flush = 0;

  connectToWIFI();

  if(WiFi.status() == WL_CONNECTED) {
    http.begin(server_url.c_str());
    int http_response_code = http.GET();

    if(http_response_code > 0) {
      payload = http.getString();
      // Serial.println(payload);
      extracted_water_value = extractValue(payload);
      Serial.println(extracted_water_value);
      price_per_5000_gallons = extracted_water_value.toDouble();
      price_per_flush = price_per_5000_gallons / 5000;
      http.end();
      disconnectFromWIFI();

      return price_per_flush;
    }
    else {
      Serial.print("Error Code: ");
      Serial.println(http_response_code);
    }
  }
  else {
    Serial.println("Failed to Connect to WIFI, Unable to Perform a GET Request");
  }
  http.end();
  disconnectFromWIFI();

  // static price if GET request fails
  price_per_5000_gallons = 45.88;
  price_per_flush = price_per_5000_gallons / 5000;

  return price_per_flush;
}

struct tm getCurrentTime() {

  connectToWIFI();
  configTime(gmt_offset_sec, daylight_offset_sec, ntp_server);
  struct tm timeinfo;
  int attempts = 0;
  int max_attempts = 3;
  while(attempts < max_attempts)
  {
    if(getLocalTime(&timeinfo)) {
      disconnectFromWIFI();
      return timeinfo;
    }
    // Serail.println("Failed to get time, trying again");
    delay(500);
    attempts++;
  }
  disconnectFromWIFI();
  timeinfo = {};
  return timeinfo;
}

void sendNtfyNotification(String message) {
  connectToWIFI();
  if(WiFi.status() != WL_CONNECTED) {
    Serial.println("Notification Did Not Send Due To Not Being Connected To WIFI");
    return;
  }
  HTTPClient http;
  String url = "https://ntfy.sh/" + ntfy_topic;
  http.begin(url);

  http.addHeader("Title", "Toilet Flush Calculator");
  http.addHeader("Content-Type", "text/plain");

  // String message = "Toilet Has Been Flushed";
  // Serial.println("Sending: " + message);

  message = message + "\nTotal Price: " + String(total_price, 5) + "\nNumber Of Daily Flushes: " + String(num_of_daily_flushes);
  int httpResponseCode = http.POST(message);

  // if (httpResponseCode > 0) {
  //   Serial.printf("ntfy response: %d\n", httpResponseCode);
  // } 
  // else {
  //   Serial.printf("Error sending: %s\n", http.errorToString(httpResponseCode).c_str());
  // }

  http.end();
  disconnectFromWIFI();
}

bool checkIfDayPass(struct tm &timeinfo) {
  int current_day = timeinfo.tm_mday;
  int current_month = timeinfo.tm_mon;
  int current_year = timeinfo.tm_year;

  if(current_year > setup_year) {
    setup_year = current_year;
    setup_month = current_month;
    setup_day = current_day;
    return true;
  }

  if(current_month > setup_month) {
    setup_month = current_month;
    setup_day = current_day;
    return true;
  }
  if(current_day > setup_day) {
    setup_day = current_day;
    return true;
  }

  return false;
}

double* readFileContents() {
  static double arr[2] = {0.00, 0.00};
  int counter = 0;
  if(!SPIFFS.exists(file_path)) {
    arr[0] = 0.00;
    arr[1] = 0.00;
    return arr;
  }

  File file = SPIFFS.open(file_path, "r");
  if(!file) {
    return arr;
  }
  while(file.available() && counter < 2)
  {
    String line = file.readStringUntil('\n');
    arr[counter] = line.toDouble();
    counter++;
  }
  file.close();
  return arr;
}

void writeFileContents(double total_price, double num_of_daily_flushes) {
  File file = SPIFFS.open(file_path, "w");
  if(!file) {
    return;
  }

  file.printf("%.5f\n", total_price);
  file.println(num_of_daily_flushes);
  file.close();
}

// void buttonISR() {
//   reset_flag = true;
// }

void  setup() {
  Serial.begin(115200);
   // Initialize the LCD connected 
  lcd.begin();
  // Turn on the backlight on LCD. 
  lcd.backlight();
  // Set up button ISR 
  // pinMode(9, INPUT_PULLUP);
  // attachInterrupt(digitalPinToInterrupt(9), buttonISR, FALLING);

  struct tm current_time = getCurrentTime();     
  setup_day = current_time.tm_mday;
  setup_month = current_time.tm_mon;
  setup_year = current_time.tm_year;
  most_recent_price = findPricePerFlush();

  if(!SPIFFS.begin(true)) {
    Serial.println("Failed to Mount SPIFFS, using default values");
  }
  double *spiff_values = readFileContents();
  total_price = spiff_values[0];
  num_of_daily_flushes = (int)spiff_values[1];
  // SPIFFS.format();
}
 
void  loop() {
    time_counter++;

    if(time_counter > 20) { // ~every minute or so it will check
      time_counter = 0;
      struct tm current_time = getCurrentTime();
      if(current_time.tm_year > 100) {
        if(checkIfDayPass(current_time)) {
          num_of_daily_flushes = 0;
          most_recent_price = findPricePerFlush();
        }
      }
    }

    lcd.setCursor(2, 0);
    lcd.print("Total Price: ");
    lcd.setCursor(2, 1);
    lcd.print("$");

    lcd.setCursor(3, 1);
    int water_sensor_value = analogRead(water_sensor_pin);

    Serial.print("Water Level: ");
    Serial.println(water_sensor_value);
    // if(reset_flag) {
    //   reset_flag = false;
    //   sendNtfyNotification("Toilet Calculator Has Been Reset");
    //   total_price = 0.00;
    //   num_of_daily_flushes = 0;
    //   writeFileContents(total_price, num_of_daily_flushes);
    // }
    // if numbers are very different and sometimes residual water messes with it so needs tuning
    // double check timing to make sure water rises back above this in time to not double count
    if(water_sensor_value < 1700) {
      // Serial.println("Flush happend");
      total_price += most_recent_price;
      // Serial.println(total_price);
      num_of_daily_flushes += 1;
      sendNtfyNotification("Toilet Has Been Flushed");
      lcd.print(total_price, 5);
      writeFileContents(total_price, num_of_daily_flushes);
      delay(18500);
      // delay(1000);
      // Serial.println("Resume tracking");
    }

    if(water_sensor_value > 1700) {
      lcd.print(total_price, 5);
    }
    delay(1500);
    lcd.setCursor(1, 0);
    lcd.print("Number Of Daily");
    lcd.setCursor(1, 1);
    lcd.print(String("Flushes: ") + String(num_of_daily_flushes));

    delay(1500);
    lcd.clear();
    delay(150);
}