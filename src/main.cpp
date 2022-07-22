#include <list>
#include "config.h"
#include "debug.h"
#include <sml/sml_file.h>
#include "Sensor.h"
#include <IotWebConf.h>
#include "EEPROM.h"
#include <ESP8266WiFi.h>

IotWebConfHtmlFormatProvider htmlFormatProviderInstance;
IotWebConfHtmlFormatProvider *htmlFormatProvider = &htmlFormatProviderInstance;

std::list<Sensor *> *sensors = new std::list<Sensor *>();

void wifiConnected();
void configSaved();
void checkTimeout();

DNSServer dnsServer;
WebServer server(80);
HTTPUpdateServer httpUpdater;
WiFiClient net;

IotWebConf iotWebConf(WIFI_AP_SSID, &dnsServer, &server, WIFI_AP_DEFAULT_PASSWORD, CONFIG_VERSION);
boolean needReset = false;
boolean connected = false;

String currentpower;

double energy_1_8_0;
double energy_1_8_0_old;

double energy_2_8_0;
double energy_2_8_0_old;

int PowerPositiv;

#define AquireyTimeoutms 120000		  // 5min
unsigned long GlobalDataAquiredTimer; // Watchdog timer to Reset SMLReader if no data have been aquired for 2 min

void process_message(byte *buffer, size_t len, Sensor *sensor)
{
	// Parse
	sml_file *file = sml_file_parse(buffer + 8, len - 16);

	for (int i = 0; i < file->messages_len; i++)
	{
		sml_message *message = file->messages[i];
		if (*message->message_body->tag == SML_MESSAGE_GET_LIST_RESPONSE)
		{
			sml_list *entry;
			sml_get_list_response *body;
			body = (sml_get_list_response *)message->message_body->data;
			for (entry = body->val_list; entry != NULL; entry = entry->next)
			{
				if (!entry->value)
				{ // do not crash on null value
					continue;
				}
				if (entry->value->type == SML_TYPE_OCTET_STRING)
				{
					continue;
				}
				else if (entry->value->type == SML_TYPE_BOOLEAN)
				{
					continue;
				}
				else if (((entry->value->type & SML_TYPE_FIELD) == SML_TYPE_INTEGER) ||
						 ((entry->value->type & SML_TYPE_FIELD) == SML_TYPE_UNSIGNED))
				{
					char buffer[255];
					double value = sml_value_to_double(entry->value);
					int scaler = (entry->scaler) ? *entry->scaler : 0;
					int prec = -scaler;
					if (prec < 0)
						prec = 0;
					value = value * pow(10, scaler);
					if ((int(entry->obj_name->str[2]) == 1) && (int(entry->obj_name->str[3]) == 8) && (int(entry->obj_name->str[4]) == 0))
					{
						energy_1_8_0 = value;
						if (energy_1_8_0_old != 0)
						{
							if (energy_1_8_0 > energy_1_8_0_old)
							{
								PowerPositiv = 1;
							}
						}
						energy_1_8_0_old = energy_1_8_0;
					}
					if ((int(entry->obj_name->str[2]) == 2) && (int(entry->obj_name->str[3]) == 8) && (int(entry->obj_name->str[4]) == 0))
					{
						energy_2_8_0 = value;
						if (energy_2_8_0_old != 0)
						{
							if (energy_2_8_0 > energy_2_8_0_old)
							{
								PowerPositiv = -1;
							}
						}
						energy_2_8_0_old = energy_2_8_0;
					}
					if ((int(entry->obj_name->str[2]) == 15) && (int(entry->obj_name->str[3]) == 7) && (int(entry->obj_name->str[4]) == 0))
					{
						value = value * PowerPositiv;
						sprintf(buffer, "%.*f", prec, value);
						currentpower = String(buffer);
					}
				}
			}
		}
	}

	DEBUG_SML_FILE(file);

	// free the malloc'd memory
	sml_file_free(file);
}

void json_sml_data()
{
	server.sendHeader("Access-Control-Allow-Origin", "*");
	/* 	129-129:199.130.3*255#EMH#
		1-0:0.0.9*255#08 0c 2a ec 2d 4c 53 6e #

		1-0:1.8.0*255#38972866.7#Wh
		1-0:2.8.0*255#25010290.9#Wh
		1-0:1.8.1*255#38972866.7#Wh
		1-0:2.8.1*255#25010290.9#Wh
		1-0:1.8.2*255#0.0#Wh
		1-0:15.7.0*255#306.1#W
	 */

	server.send(200, "application/json", "{\"Power\": \"" + currentpower + "\"}");

	// Reset Timer
	GlobalDataAquiredTimer = millis();
}

void setup()
{
	// Setup debugging stuff
	SERIAL_DEBUG_SETUP(115200);

#ifdef DEBUG
	// Delay for getting a serial console attached in time
	delay(2000);
#endif

	// Setup reading heads
	DEBUG("Setting up %d configured sensors...", NUM_OF_SENSORS);
	const SensorConfig *config = SENSOR_CONFIGS;
	for (uint8_t i = 0; i < NUM_OF_SENSORS; i++, config++)
	{
		Sensor *sensor = new Sensor(config, process_message);
		sensors->push_back(sensor);
	}
	DEBUG("Sensor setup done.");

	// Initialize publisher
	// Setup WiFi and config stuff
	DEBUG("Setting up WiFi and config stuff.");

	iotWebConf.setConfigSavedCallback(&configSaved);
	iotWebConf.setWifiConnectionCallback(&wifiConnected);
	iotWebConf.setupUpdateServer(&httpUpdater);

	boolean validConfig = iotWebConf.init();
	if (!validConfig)
	{
		DEBUG("Missing or invalid config. MQTT publisher disabled.");
	}
	server.on("/", []
			  { iotWebConf.handleConfig(); });
	server.on("/api/data/json", []
			  { json_sml_data(); });
	server.onNotFound([]()
					  { iotWebConf.handleNotFound(); });

	DEBUG("Setup done.");

	GlobalDataAquiredTimer = millis();

	ESP.wdtEnable(5000);
}

void loop()
{
	ESP.wdtFeed();
	checkTimeout();

	if (needReset)
	{
		// Doing a chip reset caused by config changes
		DEBUG("Rebooting after 1 second.");
		delay(1000);
		ESP.restart();
	}

	// Execute sensor state machines
	for (std::list<Sensor *>::iterator it = sensors->begin(); it != sensors->end(); ++it)
	{
		(*it)->loop();
	}
	iotWebConf.doLoop();
	yield();
}

void configSaved()
{
	DEBUG("Configuration was updated.");
	needReset = true;
}

void wifiConnected()
{
	DEBUG("WiFi connection established.");
	connected = true;
}

void checkTimeout()
{
	if ((millis() - GlobalDataAquiredTimer) > AquireyTimeoutms)
	{
		needReset = true;
	}
}