/*

	Digital Tensio Sensor sketch for LOLIN(WEMOS) D1 R2 & Mini and a MPXHZ6400AC6T1 pressure sensor
	by Carl Bisshop (Cabis)
	
	Blumat: https://www.blumat.nl/product/blumat-maxi-zonder-druppelslang-en-t-stuk/

	Remove inner cup from Blumat
	Cut tip of white inner cup (which will show a hole the sensor will fit in)
	Cut head of outer cup so after assembly the a part of the white cup and sensor sticks out a few mm
	
	3d print for parts: https://www.thingiverse.com/thing:3360573
	
	Glue the resembled blumat cup into the house (treat should point to small hole)
	Position the Wemos with the reset button upwards and the metal housing facing the holes
	
	Sensor: http://www.aliexpress.com/wholesale?SearchText=%E2%80%A2%09MPXHZ6400AC6T1
	Data sheet: https://www.nxp.com/docs/en/data-sheet/MPXHZ6400A.pdf
	
	Sensor Connections
	
	Wemos	MPXHZ6400
	 +5V	   Vs
	 GND	   GND
	 A0		   vOut
	 
	Sensor output will not exceed 1V when used as a tensio meter for plant medium
	
	If used as a pressure sensor with pressures above 1 Bar you could hit > 3V output of the sensor which will blow A0! (not tested though)

*/

#include <EEPROM.h>
int EEPROM_START_ADDRESS = 0;

#include <WiFiManager.h>
#include <WiFiClient.h>

#include <ESP8266HTTPClient.h>
#include <ESP8266HTTPUpdateServer.h>

#include <PubSubClient.h>

#include <ArduinoJson.h>

uint16_t	 EEPROM_INITIALIZED = 0xBFFE;
String title = "Tensio Sensor V1.1";

struct __attribute__ ((packed)) _sensor {

	int			initID;						/* ID to check if EEPROM contains config */
	uint16_t	interval;					/* MQTT Send interval in Seconds */
	char		name[32];					/* Sensor Name, used in MQTT Topic */
	char		topic[32];					/* MQTT publish Topic */
	boolean		mqtt_enabled;				/* Enable / Disable MQTT */
	char		mqtt_server_ip[15];			/* MQTT Server IP */
    int         mqtt_port;					/* MQTT Port */
	char		mqtt_username[32];			/* MQTT Username */
	char		mqtt_password[32];			/* MQTT Password */
	
};
_sensor sensor;

char publish_topic[128];

const int BUFFER_SIZE = JSON_OBJECT_SIZE(10);
#define MQTT_MAX_PACKET_SIZE 512

/* Default AP Name & Secret */
#define sensorAPname "Tensio"
#define sensorAPpass "Remote123!"

#define HTTP_PORT	80

#define SENSOR_PORT A0			/* Analog Sensor Port */
uint32_t	delayTimer	= 0;


#define   AVERAGE_COUNT 50			/* Number of Samples for Average */
int		  raw_sensor_values[AVERAGE_COUNT];

WiFiClient espClient;
PubSubClient MQTT_Client(espClient);

ESP8266WebServer httpServer ( HTTP_PORT );
ESP8266HTTPUpdateServer httpUpdater;

/* Write bytes to EEPROM */
void writeBlock(void *payload, uint16_t payloadSize, int startAddr) {

	uint8_t* buff = reinterpret_cast<uint8_t*>(payload);

	for (int ii=0; ii<payloadSize; ii++) { EEPROM.write(startAddr + ii, buff[ii]); }
	if (  ! EEPROM.commit() )  { Serial.printf("Commit Error\n"); }
}

/* Read bytes from EEPROM */
void readBlock(void *payload, uint16_t payloadSize, int startAddr) {

	uint8_t* buff = reinterpret_cast<uint8_t*>(payload);
	for (int ii=0; ii<payloadSize; ii++) { buff[ii] = EEPROM.read(startAddr + ii); }
}

/* Read config from EEPROM */
void readEEProm()
{

	readBlock(&sensor,  sizeof sensor, EEPROM_START_ADDRESS);
	
	updateTopic();

	Serial.printf("Sensor => %s\n", sensor.name);
	Serial.printf("MQTT Server => %s:%d\n", sensor.mqtt_server_ip, sensor.mqtt_port);
	
}

/* Write config to EEPROM */
void writeEEProm()
{

	writeBlock(&sensor,  sizeof sensor, EEPROM_START_ADDRESS);
	readEEProm();
}


/* Initialize EEPROM & write default config if not exists */
void initEEPROM(bool reset = false)
{
	
	uint16_t id;
	
	readBlock(&id, sizeof id, EEPROM_START_ADDRESS);

	if ( id != EEPROM_INITIALIZED || reset )
	{


		// Initialize the EEProm  with some defaults
		sensor.initID			= EEPROM_INITIALIZED;

		strcpy(sensor.name, "Carolina Reaper");
		sensor.interval = 300;
		strcpy(sensor.mqtt_server_ip, "127.0.0.1");
		strcpy(sensor.mqtt_username, "");
		strcpy(sensor.mqtt_password, "");
		strcpy(sensor.topic, "/pepper/tensio");
		
		sensor.mqtt_enabled = false;
		sensor.mqtt_port	= 1883;

		writeEEProm();
	}
	
	readEEProm();
	
}


/* Calculate average reading */
uint16_t average_reading()
{

	uint16_t sum = 0;
	for (uint8_t ii=0; ii<AVERAGE_COUNT; ii++) sum += raw_sensor_values[ii];

	return (uint16_t) (sum / AVERAGE_COUNT);
	
}


/* Convert Sensor value to mBar */
/* See sensor data sheet for formula */
float get_mbar()
{
	
	float volts, kPa, mBar;

	uint16_t sensorValue = average_reading();

	volts  = sensorValue * (3.2 / 1023.0);
	kPa    = ( volts - (0.00842 * 5) ) /  (0.002421 * 5);
	mBar   = ( 100 - kPa ) * 10;

	return  mBar;
}

/* MQTT Publish */
void publish_to_MQTT() {

	/* Sample jSon send to MQTT server: {"raw":4,"mbar":1024} */

	if ( sensor.mqtt_enabled )
	{
		
		StaticJsonBuffer<BUFFER_SIZE> jsonBuffer;
		JsonObject& root = jsonBuffer.createObject();

		root["raw"]		= average_reading();
		root["mbar"]	= int( get_mbar() );

		char buffer[root.measureLength() + 1];

		root.printTo(buffer, sizeof(buffer));

		if ( ! MQTT_Client.publish(publish_topic, buffer, true) ) {
			Serial.println("Error Publishing ...");
		}
		
	}
	
}


/* Reconnect to MQTT Server */
void reconnect_mqttserver() {

	if ( sensor.mqtt_enabled )
	{
		
		if (MQTT_Client.connect(sensor.name, sensor.mqtt_username, sensor.mqtt_password)) {
			if ( MQTT_Client.subscribe(publish_topic) )
			{
				Serial.printf("Subscribed to %s\n", publish_topic );
				publish_to_MQTT();
			} else {
				Serial.printf("Subscription to %s failed!\n", publish_topic );
			}
		} else {
			Serial.printf("Connection to MQTT Server %s:%d failed with rc=%d.\n", sensor.mqtt_server_ip, sensor.mqtt_port, MQTT_Client.state());
		}
	}
	
}


/* Config Sensor */
void config()
{
	
		
	int args = httpServer.args();
	
	
	if ( args >= 6 )
	{
		
		Serial.println("Config Change Requested");

		httpServer.arg("sn").toCharArray(sensor.name, sizeof sensor.name);
		sensor.interval = httpServer.arg("si").toInt();

		httpServer.arg("mu").toCharArray(sensor.mqtt_username, sizeof sensor.mqtt_username);
		httpServer.arg("mpw").toCharArray(sensor.mqtt_password, sizeof sensor.mqtt_password);
		
		httpServer.arg("ms").toCharArray(sensor.mqtt_server_ip, sizeof sensor.mqtt_server_ip);
		sensor.mqtt_port = httpServer.arg("mp").toInt();
		httpServer.arg("mt").toCharArray(sensor.topic, sizeof sensor.topic);
		
		sensor.mqtt_enabled = httpServer.hasArg("me");
		
		writeEEProm();
		
		updateTopic();
		delayTimer = 0;
	}

	/* HTML Form for Config */	
	String mqqt_enabled = sensor.mqtt_enabled ? " checked" : "";
	String message = "<h1>" + title + "</h1>\
	<h4>by Carl Bisshop</h4>\
	<form method='POST'>\
	<table>\
	<tr><td>Sensor Reading</td><td><b><i>" + String( get_mbar() ) + "</b></i> mBar</td></tr>\
	<tr><td colspan='2'></td></tr>\
	<tr><td>Sensor Name</td><td><input name='sn' type='text' maxlength='32' value='" + sensor.name +"'></td></tr>\
	<tr><td>Sensor Interval</td><td><input name='si' min='10' max='65534' type='number' value='"+ sensor.interval +"'> Seconds</td></tr>\
	<tr><td colspan='2'></td></tr>\
	<tr><td>Enable MQTT</td><td><input name='me' type='checkbox' value=''"+ mqqt_enabled +"></td></tr>\
	<tr><td colspan='2'></td></tr>\
	<tr><td>MQTT Topic</td><td><input name='mt' type='text' maxlength='32' value='"+ sensor.topic +"'></td></tr>\
	<tr><td colspan='2'></td></tr>\
	<tr><td>MQTT Server</td><td><input name='ms' type='text' value='"+ sensor.mqtt_server_ip +"'></td></tr>\
	<tr><td>MQTT Port</td><td><input name='mp' min='1' max='65534' type='number' value='"+ sensor.mqtt_port +"'></td></tr>\
	<tr><td colspan='2'></td></tr>\
	<tr><td>MQTT User</td><td><input name='mu' type='text' maxlength='32' value='"+ sensor.mqtt_username +"'></td></tr>\
	<tr><td>MQTT Secret</td><td><input name='mpw' type='password' maxlength='32' value='"+ sensor.mqtt_password +"'></td></tr>\
	<tr><td colspan='2' align='center'><input type='submit' value='Save'></td></tr>\
	</table>\
	</form>\
	";

	httpServer.send ( 200, "text/html", message );
	
}

void handlemBar()
{
	httpServer.send ( 200, "text/html", String( get_mbar() ) );
}

void handleNotFound() {

	String message	= "Requested page not found!";
	httpServer.send ( 404, "text/html", message );
}

void handleRoot() {
	
	httpServer.send ( 200, "text/html", title );
}


/* Update Topic */
void updateTopic()
{
	
	sprintf(publish_topic,"%s/%s", sensor.topic, sensor.name);
	Serial.printf("Update Topic To %s\n", publish_topic);

	MQTT_Client.disconnect(); // Will force reconnect to MQTT with new topic
	
}

void setup()
{
	
	Serial.begin(115200);
	
	WiFiManager wifiManager;
	wifiManager.autoConnect(sensorAPname, sensorAPpass);

	httpUpdater.setup(&httpServer, "/firmware-update");
	httpServer.begin();

    httpServer.on ( "/", handleRoot );
    httpServer.on ( "/config", config );
    httpServer.on ( "/mbar", handlemBar );
    httpServer.onNotFound(handleRoot);

	uint32_t EEPROM_SIZE = sizeof sensor;
	EEPROM.begin(EEPROM_SIZE /* Bytes */);
	
	initEEPROM();

	MQTT_Client.setServer(sensor.mqtt_server_ip, sensor.mqtt_port);

	pinMode(SENSOR_PORT, INPUT);

	delay(20);  /* Sensor warm up */
	uint16_t initValue  = analogRead(SENSOR_PORT);
	for (uint8_t ii=0; ii<AVERAGE_COUNT; ii++) raw_sensor_values[ii] = initValue;

}


void loop() {
	
	static uint8_t  average_ndx	= 0;


	if  ( millis() > delayTimer )
	{
		
		if (! MQTT_Client.connected() && sensor.mqtt_enabled)
		{
			Serial.println("MQTT Client Not Connected");
			reconnect_mqttserver();
		} else {
			publish_to_MQTT();
		}
		
		delayTimer = (  sensor.interval * 1000 ) + millis();
		Serial.printf("Sensor [%s] - %d mBar\n", sensor.name, average_reading());
	}
	
	httpServer.handleClient();
		
	raw_sensor_values[average_ndx] = analogRead(SENSOR_PORT);


	average_ndx++;
	if (average_ndx >= AVERAGE_COUNT) average_ndx=0;
	
	delay(30);

}