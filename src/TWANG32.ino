/*
  TWANG32 - An ESP32 port of TWANG
  (c) B. Dring 3/2018
  License: Creative Commons 4.0 Attribution - Share Alike

  TWANG was originally created by Critters
  https://github.com/Critters/TWANG

  It was inspired by Robin Baumgarten's Line Wobbler Game

  Recent Changes
  - Updated to move FastLEDshow to core 0
  Fixed Neopixel
    - Used latest FastLED library..added compile check
    - Updated to move FastLEDshow to core 0
    - Reduced MAX_LEDS on Neopixel
    - Move some defines to a separate config.h file to make them accessible to other files
  - Changed volume typo on serial port settings menu

  Project TODO
    - Make the strip type configurable via serial and wifi


  Usage Notes:
    - Changes to LED strip and other hardware are in config.h
    - Change the strip type to what you are using and compile/load firmware
    - Use Serial port or Wifi to set your strip length.
*/

//

#define VERSION "2018-06-28"

#include <FastLED.h>
#include <Wire.h>
#include "Arduino.h"

// twang files
#include "config.h"
#include "twang_mpu.h"
#include "enemy.h"
#include "particle.h"
#include "spawner.h"
#include "lava.h"
#include "boss.h"
#include "conveyor.h"
#include "iSin.h"
#include "sound.h"
#include "settings.h"
#include "wifi_ap.h"
#include "samples.h"

#if defined(FASTLED_VERSION) && (FASTLED_VERSION < 3001000)
#error "Requires FastLED 3.1 or later; check github for latest code."
#endif

#define DIRECTION 1
#define USE_GRAVITY 0  // 0/1 use gravity (LED strip going up wall)

// GAME
long previousMillis = 0; // Time of the last redraw
int levelNumber = 0;

#define TIMEOUT 20000 // time until screen saver in milliseconds

int joystickTilt = 0;   // Stores the angle of the joystick
int joystickWobble = 0; // Stores the max amount of acceleration (wobble)

// WOBBLE ATTACK
#define DEFAULT_ATTACK_WIDTH 70 // Width of the wobble attack, world is 1000 wide
int attack_width = DEFAULT_ATTACK_WIDTH;
#define ATTACK_DURATION 500 // Duration of a wobble attack (ms)
long attackMillis = 0;      // Time the attack started
bool attacking = 0;         // Is the attack in progress?
#define BOSS_WIDTH 40

// TODO all animation durations should be defined rather than literals
// because they are used in main loop and some sounds too.
#define STARTUP_WIPEUP_DUR 200
#define STARTUP_SPARKLE_DUR 1300
#define STARTUP_FADE_DUR 1500

#define GAMEOVER_SPREAD_DURATION 1000
#define GAMEOVER_FADE_DURATION 1500

#define WIN_FILL_DURATION 500 // sound has a freq effect that might need to be adjusted
#define WIN_CLEAR_DURATION 1000
#define WIN_OFF_DURATION 1200

Twang_MPU accelgyro = Twang_MPU(Twang_MPU::MPU_ADDR_DEFAULT);
CRGB leds[VIRTUAL_LED_COUNT];
Samples MPUAngleSamples = {0};
Samples MPUWobbleSamples = {0};
iSin isin = iSin();

// #define JOYSTICK_DEBUG  // comment out to stop serial debugging

// POOLS
#define ENEMY_COUNT 10
Enemy enemyPool[ENEMY_COUNT] = {
    Enemy(), Enemy(), Enemy(), Enemy(), Enemy(), Enemy(), Enemy(), Enemy(), Enemy(), Enemy()};

#define PARTICLE_COUNT 100
Particle particlePool[PARTICLE_COUNT] = {
    Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle(), Particle()};

#define SPAWN_COUNT 5
Spawner spawnPool[SPAWN_COUNT] = {
    Spawner(), Spawner()};

#define LAVA_COUNT 5
Lava lavaPool[LAVA_COUNT] = {
    Lava(), Lava(), Lava(), Lava()};

#define CONVEYOR_COUNT 2
Conveyor conveyorPool[CONVEYOR_COUNT] = {
    Conveyor(), Conveyor()};

Boss boss = Boss();

enum stages
{
    STARTUP,
    PLAY,
    WIN,
    DEAD,
    SCREENSAVER,
    BOSS_KILLED,
    GAMEOVER
} stage;

long stageStartTime;        // Stores the time the stage changed for stages that are time based
int playerPosition;         // Stores the player position
int playerPositionModifier; // +/- adjustment to player position
bool playerAlive;
long killTime;
int lives = LIVES_PER_LEVEL;
bool lastLevel = false;

bool gyroConnected = false;
unsigned long gyroLastCheckMs = 0;
const unsigned long GYRO_CHECK_INTERVAL_MS = 2000;

int score = 0;

#define FASTLED_SHOW_CORE 0 // -- The core to run FastLED.show()

// -- Task handles for use in the notifications
static TaskHandle_t FastLEDshowTaskHandle = 0;
static TaskHandle_t userTaskHandle = 0;

/** show() for ESP32
 *  Call this function instead of FastLED.show(). It signals core 0 to issue a show,
 *  then waits for a notification that it is done.
 */
void FastLEDshowESP32()
{
    if (userTaskHandle == 0)
    {
        // -- Store the handle of the current task, so that the show task can
        //    notify it when it's done
        userTaskHandle = xTaskGetCurrentTaskHandle();

        // -- Trigger the show task
        xTaskNotifyGive(FastLEDshowTaskHandle);

        // -- Wait to be notified that it's done
        const TickType_t xMaxBlockTime = pdMS_TO_TICKS(200);
        ulTaskNotifyTake(pdTRUE, xMaxBlockTime);
        userTaskHandle = 0;
    }
}

/** show Task
 *  This function runs on core 0 and just waits for requests to call FastLED.show()
 */
void FastLEDshowTask(void *pvParameters)
{
    // -- Run forever...
    for (;;)
    {
        // -- Wait for the trigger
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // -- Do the show (synchronously)
        FastLED.show();

        // -- Notify the calling task
        xTaskNotifyGive(userTaskHandle);
    }
}

void setup()
{
    Serial.begin(115200);
    Serial.print("\r\nTWANG32 VERSION: ");
    Serial.println(VERSION);

    settings_init(); // load the user settings from EEPROM

    Wire.begin();
    accelgyro.initialize();
    gyroConnected = accelgyro.verify();
    gyroLastCheckMs = millis();
    Serial.printf("Gyro is %sconncted!\n", gyroConnected ? "" : "NOT ");

#ifdef USE_NEOPIXEL
    Serial.print("\r\nCompiled for WS2812B (Neopixel) LEDs");
    FastLED.addLeds<LED_TYPE, DATA_PIN>(leds, MAX_LEDS);
#endif

#ifdef USE_APA102
    Serial.print("\r\nCompiled for APA102 (Dotstar) LEDs");
    FastLED.addLeds<LED_TYPE, DATA_PIN, CLOCK_PIN, LED_COLOR_ORDER>(leds, MAX_LEDS);
#endif

    FastLED.setBrightness(user_settings.led_brightness);
    FastLED.setDither(1);

    // -- Create the ESP32 FastLED show task
    xTaskCreatePinnedToCore(FastLEDshowTask, "FastLEDshowTask", 2048, NULL, 2, &FastLEDshowTaskHandle, FASTLED_SHOW_CORE);

    sound_init();

    ap_setup();

    stage = STARTUP;
    stageStartTime = millis();
    lives = user_settings.lives_per_level;
}

void loop()
{
    long mm = millis();

    ap_client_check(); // check for web client
	if (Serial.available())
	{
		settings_param_t param = settings_processSerial(Serial.read());
        if (param.code == 'V' && param.hasValue)
        {
            Serial.printf("Skipping to level %d...\n", param.newValue);
            levelNumber = param.newValue;
            loadLevel(levelNumber);
        }
        else
        {
            settings_set(param);
        }
	}

    if (stage == PLAY)
    {
        if (attacking)
        {
            SFXattacking();
        }
        else
        {
            SFXtilt(joystickTilt);
        }
    }
    else if (stage == DEAD)
    {
        SFXdead();
    }

    if (mm - previousMillis >= MIN_REDRAW_INTERVAL)
    {
        if (gyroConnected)
        {
            gyroConnected = getInput();
            if (!gyroConnected)
                Serial.println("Gyro disconnected!");
        }
        else if (millis() - gyroLastCheckMs > GYRO_CHECK_INTERVAL_MS)
        {
            accelgyro.initialize();
            gyroConnected = accelgyro.verify();
            gyroLastCheckMs = millis();
            if (gyroConnected)
                Serial.println("Gyro connected!");
        }

        long frameTimer = mm;
        previousMillis = mm;

        if (abs(joystickTilt) > user_settings.joystick_deadzone)
        {
            lastInputTime = mm;
            if (stage == SCREENSAVER)
            {
                levelNumber = -1;
                stageStartTime = mm;
                stage = WIN;
                FastLED.setBrightness(user_settings.led_brightness);
                Serial.println("Woke up from screensaver, going to game...");
            }
        }
        else
        {
            if (lastInputTime + TIMEOUT < mm && stage != SCREENSAVER)
            {
                stage = SCREENSAVER;
                // dim when going to screensaver
                FastLED.setBrightness(constrain(user_settings.led_brightness / 4, MIN_BRIGHTNESS, MAX_BRIGHTNESS));
                Serial.println("Going to screensaver...");
            }
        }

        if (stage == SCREENSAVER)
        {
            screenSaverTick();
        }
        else if (stage == STARTUP)
        {
            if (stageStartTime + STARTUP_FADE_DUR > mm)
            {
                tickStartup(mm);
            }
            else
            {
                SFXcomplete();
                levelNumber = 0;
                loadLevel(0);
            }
        }
        else if (stage == PLAY)
        {
            // PLAYING
            if (attacking && attackMillis + ATTACK_DURATION < mm)
                attacking = 0;

            // If not attacking, check if they should be
            if (!attacking && joystickWobble >= user_settings.attack_threshold)
            {
                attackMillis = mm;
                attacking = 1;
            }

            // If still not attacking, move!
            playerPosition += playerPositionModifier;
            if (!attacking)
            {
                SFXtilt(joystickTilt);
                // int moveAmount = (joystickTilt/6.0);  // 6.0 is ideal at 16ms interval (6.0 / (16.0 / MIN_REDRAW_INTERVAL))
                int moveAmount = (joystickTilt / (6.0)); // 6.0 is ideal at 16ms interval
                if (DIRECTION)
                    moveAmount = -moveAmount;
                moveAmount = constrain(moveAmount, -MAX_PLAYER_SPEED, MAX_PLAYER_SPEED);

                playerPosition -= moveAmount;
                if (playerPosition < 0)
                    playerPosition = 0;

                // stop player from leaving if boss is alive
                if (boss.Alive() && playerPosition >= VIRTUAL_LED_COUNT) // move player back
                    playerPosition = 999;                                //(user_settings.led_count - 1) * (1000.0/user_settings.led_count);

                if (playerPosition >= VIRTUAL_LED_COUNT && !boss.Alive())
                {
                    // Reached exit!
                    levelComplete();
                    return;
                }
            }

            if (inLava(playerPosition))
            {
                die();
            }

            // Ticks and draw calls
            FastLED.clear();
            tickConveyors();
            tickSpawners();
            tickBoss();
            tickLava();
            tickEnemies();
            drawPlayer();
            drawAttack();
            drawExit();
        }
        else if (stage == DEAD)
        {
            // DEAD
            FastLED.clear();
            tickDie(mm);
            if (!tickParticles())
            {
                loadLevel(levelNumber);
            }
        }
        else if (stage == WIN)
        {
            // LEVEL COMPLETE
            tickWin(mm);
        }
        else if (stage == BOSS_KILLED)
        {
            tickBossKilled(mm);
        }
        else if (stage == GAMEOVER)
        {
            if (stageStartTime + GAMEOVER_FADE_DURATION > mm)
            {
                tickGameover(mm);
            }
            else
            {
                FastLED.clear();
                save_game_stats(false); // boss not killed

                // restart from the beginning
                stage = STARTUP;
                stageStartTime = millis();
                lives = user_settings.lives_per_level;
            }
        }

        // FastLED.show();
        FastLEDshowESP32();
    }
}

// ---------------------------------
// ------------ LEVELS -------------
// ---------------------------------
void loadLevel(int num)
{
    // leave these alone
    // FastLED.setBrightness(user_settings.led_brightness);
    updateLives();
    cleanupLevel();
    playerAlive = 1;
    lastLevel = false; // this gets changed on the boss level

    /// Defaults...OK to change the following items in the levels below
    attack_width = DEFAULT_ATTACK_WIDTH;
    playerPosition = 0;

    /* ==== Level Editing Guide ===============
    Level creation is done by adding to or editing the switch statement below.

    Add your level to the enum, then add a case statement for it in the switch.
    The order of levels in the enum determines their order in game. 

    TWANG uses a virtual 1000 LED grid. It will then scale that number to your strip, so if you
    want something in the middle of your strip use the number 500. Consider the size of your strip
    when adding features. All time values are specified in milliseconds (1/1000 of a second)

    You can add any of the following features.
    See function descriptions (comments above function implementation) for more info.

    spawnEnemy(): You can add up to 10 (ENEMY_COUNT) static or moving enemies.

    spawnSpawner(): This generates and endless source of new enemies. 
      5 (SPAWN_COUNT) pools max

    spawnLava(): You can create 5 (LAVA_COUNT) pools of lava. 
      Lava will toggle on and off in an interval. 
      Lava kills the player and enemies when on.

    spawnConveyor(): You can create 2 (CONVEYOR_COUNT) conveyors. 
      Conveyors move the player at a constant speed.

    ===== Other things you can adjust per level ================

    Player Start position (0..1000):
      playerPosition = xxx; 

    The size of the TWANG attack
      attack_width = xxx;
    */

    // TODO: Levels with conveyor and lava

    enum LEVELS {
        INTRO=0,
        ENEMY_INTRO,
        SPAWNER_INTRO,
        LAVA_INTRO,
        LAVA_MOVING,
        LAVA_SPREADING,
        ENEMY_SIN_INTRO,
        ENEMY_SIN_SWARM,
        LAVA_MOVING_UP,
        CONVEYOR_INTRO,
        CONVEYOR_ENEMIES,
        LAVA_SPREAD_FALL,
        SPAWNER_TRAIN,
        SPAWNER_TRAIN_SKINNY,
        SPAWNER_SPLIT,
        SPAWNER_SPLIT_LAVA,
        LAVA_RUN,
        CONVEYOR_ENEMY_SIN,
        CONVEYOR_ENEMY_FAST,
        BOSS, // This should always be the last valid level!
    };

    if (num < 0 || num >= BOSS)
    {
        Serial.printf("ERROR: Unknown level %d. Defaulting to starting level...\n", num);
        num = 0;
    }

    switch (num)
    {
    case INTRO: 
        playerPosition = 200;
        spawnEnemy(1, 0, 0, 0);
        break;
    case ENEMY_INTRO:
        spawnEnemy(900, 0, 1, 0);
        break;
    case SPAWNER_INTRO:
        spawnSpawner(950, 4000, 2, 0, -3000);
        break;
    case LAVA_INTRO:
        spawnLava(400, 490, 2000, 2000, 0, Lava::OFF, 0, 0);
        spawnEnemy(350, 0, 1, 0);
        spawnSpawner(950, 4500, 3, 0, -3500);
        break;
    case LAVA_MOVING:
        spawnLava(700, 800, 2000, 2000, 0, Lava::OFF, 0, -0.5);
        spawnEnemy(450, 0, 1, 0);
        spawnEnemy(950, 0, 1, 0);
        spawnSpawner(950, 4500, 3, 0, -2000);
        break;
    case LAVA_SPREADING:
        spawnLava(350, 400, 2000, 2000, 0, Lava::OFF, 0.25, 0);
        spawnLava(800, 850, 2000, 2000, 0, Lava::OFF, 0.25, 0);
        spawnEnemy(450, 0, 1, 0);
        spawnEnemy(900, 0, 2, 0);
        break;
    case ENEMY_SIN_INTRO:
        spawnEnemy(700, 1, 7, 275);
        spawnEnemy(500, 1, 5, 250);
        break;
    case ENEMY_SIN_SWARM:
        spawnEnemy(700, 1, 7, 275); // 425..975
        spawnEnemy(600, 1, 5, 300); // 300..900

        spawnEnemy(800, 1, 6, 200); // 600..1000
        spawnEnemy(450, 1, 5, 300); // 150..750

        spawnEnemy(500, 1, 7, 350); // 150..800
        spawnEnemy(450, 1, 3, 150); // 300..600
        break;
    case LAVA_MOVING_UP:
        playerPosition = 200;
        spawnLava(10, 120, 2000, 2000, 0, Lava::OFF, 0, 0.5);
        spawnEnemy(500, 0, 1, 0);
        spawnSpawner(950, 3000, 3, 0, -1000);
        break;
    case CONVEYOR_INTRO:
        spawnConveyor(100, 600, -6);
        spawnEnemy(650, 0, 0, 0);
        spawnEnemy(800, 1, 1, 0);
        break;
    case CONVEYOR_ENEMIES:
        spawnConveyor(50, 1000, 6);
        spawnEnemy(300, 0, 0, 0);
        spawnEnemy(400, 0, 0, 0);
        spawnEnemy(500, 0, 0, 0);
        spawnEnemy(600, 0, 0, 0);
        spawnEnemy(700, 0, 0, 0);
        spawnEnemy(800, 0, 0, 0);
        spawnEnemy(900, 0, 0, 0);
        break;
    case LAVA_SPREAD_FALL:
        spawnLava(400, 450, 2000, 2000, 0, Lava::OFF, 0.2, -0.5);
        spawnEnemy(350, 0, 1, 0);
        spawnSpawner(950, 5500, 3, 0, 0);
        break;
    case SPAWNER_TRAIN:
        spawnSpawner(900, 1300, 2, 0, 0);
        break;
    case SPAWNER_TRAIN_SKINNY:
        attack_width = 32;
        spawnSpawner(900, 1800, 2, 0, 0);
        break;
    case SPAWNER_SPLIT:
        spawnSpawner(550, 1500, 2, 0, 0);
        spawnSpawner(550, 1500, 2, 1, 0);
        break;
    case SPAWNER_SPLIT_LAVA:
        spawnSpawner(500, 1200, 2, 0, 0);
        spawnSpawner(500, 1200, 2, 1, 0);
        spawnLava(900, 950, 2200, 800, 2000, Lava::OFF, 0, 0);
        break;
    case LAVA_RUN:
        spawnLava(195, 300, 2000, 2000, 0, Lava::OFF, 0, 0);
        spawnLava(400, 500, 2000, 2000, 0, Lava::OFF, 0, 0);
        spawnLava(600, 700, 2000, 2000, 0, Lava::OFF, 0, 0);
        spawnSpawner(950, 3800, 4, 0, 0);
        break;
    case CONVEYOR_ENEMY_SIN:
        spawnEnemy(700, 1, 7, 275);
        spawnEnemy(500, 1, 5, 250);
        spawnSpawner(950, 5500, 4, 0, 3000);
        spawnSpawner(0, 5500, 5, 1, 10000);
        spawnConveyor(100, 900, -4);
        break;
    case CONVEYOR_ENEMY_FAST:
        spawnEnemy(800, 1, 7, 275);
        spawnEnemy(700, 1, 7, 275);
        spawnEnemy(500, 1, 5, 250);
        spawnSpawner(950, 3000, 4, 0, 3000);
        spawnSpawner(0, 5500, 5, 1, 10000);
        spawnConveyor(100, 900, -6);
        break;
    case BOSS:
        // Boss this should always be the last level
        spawnBoss();
        break;
    }
    lastInputTime = stageStartTime = millis();
    stage = PLAY;
}

void spawnBoss()
{
    lastLevel = true;
    boss.Spawn();
    moveBoss();
}

void moveBoss()
{
    int spawnSpeed = 1800;
    if (boss._lives == 2)
        spawnSpeed = 1600;
    if (boss._lives == 1)
        spawnSpeed = 1000;
    spawnPool[0].Spawn(boss._pos, spawnSpeed, 3, 0, 0);
    spawnSpawner(boss._pos, spawnSpeed, 3, 1, 0);
}

/* ======================== spawn Functions =====================================

   The following spawn functions add items to pools by looking for an unactive
   item in the pool. You can only add as many as the ..._COUNT. Additonal attemps
   to add will be ignored.

   ==============================================================================
*/
// @param pos: Where the enemy starts (in game coordinates, usually 0..1000)
// @param dir: If it moves, what direction does it go 0=down, 1=away
// @param speed: How fast does it move. Typically 1 to 4.
// @param wobble: 0=regular movement, >0 set length of bouncing back and forth in a sine pattern
void spawnEnemy(int pos, int dir, int speed, int wobble)
{
    for (int e = 0; e < ENEMY_COUNT; e++)
    { // look for one that is not alive for a place to add one
        if (!enemyPool[e].Alive())
        {
            enemyPool[e].Spawn(pos, dir, speed, wobble);
            enemyPool[e].playerSide = pos > playerPosition ? 1 : -1;
            return;
        }
    }
}

// @param pos: The location the enemies with be generated from (in game coordinates, usually 0..1000)
// @param rate_ms: The time in milliseconds between each new enemy
// @param speed: How fast they move. Typically 1 to 4.
// @param dir: Directions they go 0=down, 1=towards goal
// @param startOffset_ms: The delay in milliseconds before the first enemy (added to rate, can be negative)
void spawnSpawner(int pos, int rate_ms, int speed, int dir, int startOffset_ms)
{
    for (int s = 0; s < SPAWN_COUNT; s++)
    {
        if (!spawnPool[s].Alive())
        {
            spawnPool[s].Spawn(pos, rate_ms, speed, dir, startOffset_ms);
            return;
        }
    }
}

// @param left: the lower end of the lava pool (in game coordinates, usually 0..1000)
// @param right: the upper end of the lava pool
// @param ontime: How long the lava stays on in milliseconds
// @param offtime: How long the lava is off in milliseconds
// @param offset: How long (ms) after the level starts before the lava turns on, use this to create patterns with multiple lavas
// @param state: does it start on or off (Lava::ON or Lava::OFF)
// @param grow: This specifies the rate of growth. Use 0 for no growth. Reasonable growth is 0.1 to 0.5
// @param flow: This specifies the rate/direction of flow. Reasonable numbers are 0.2 to 0.8 (positive or negative)
void spawnLava(int left, int right, int ontime, int offtime, int offset, int state, float grow, float flow)
{
    for (int i = 0; i < LAVA_COUNT; i++)
    {
        if (!lavaPool[i].Alive())
        {
            lavaPool[i].Spawn(left, right, ontime, offtime, offset, state, grow, flow);
            return;
        }
    }
}

// @param startPoint: The close end of the conveyor (in game coordinates 0..1000)
// @param endPoint: The far end of the conveyor
// @param dir: positive = away, negative = towards you (must be less than +/- MAX_PLAYER_SPEED=10)
void spawnConveyor(int startPoint, int endPoint, int dir)
{
    for (int i = 0; i < CONVEYOR_COUNT; i++)
    {
        if (!conveyorPool[i]._alive)
        {
            conveyorPool[i].Spawn(startPoint, endPoint, dir);
            return;
        }
    }
}

void cleanupLevel()
{
    for (int i = 0; i < ENEMY_COUNT; i++)
    {
        enemyPool[i].Kill();
    }
    for (int i = 0; i < PARTICLE_COUNT; i++)
    {
        particlePool[i].Kill();
    }
    for (int i = 0; i < SPAWN_COUNT; i++)
    {
        spawnPool[i].Kill();
    }
    for (int i = 0; i < LAVA_COUNT; i++)
    {
        lavaPool[i].Kill();
    }
    for (int i = 0; i < CONVEYOR_COUNT; i++)
    {
        conveyorPool[i].Kill();
    }
    boss.Kill();
}

void levelComplete()
{
    stageStartTime = millis();
    stage = WIN;

    if (lastLevel)
    {
        stage = BOSS_KILLED;
    }
    if (levelNumber != 0) // no points for the first level
    {
        score = score + (lives * 10); //
    }
}

void nextLevel()
{
    levelNumber++;

    if (lastLevel)
    {
        stage = STARTUP;
        stageStartTime = millis();
        lives = user_settings.lives_per_level;
    }
    else
    {
        lives = user_settings.lives_per_level;
        loadLevel(levelNumber);
    }
}

void die()
{
    playerAlive = 0;
    if (levelNumber > 0)
        lives--;

    if (lives == 0)
    {
        stage = GAMEOVER;
        stageStartTime = millis();
    }
    else
    {
        for (int p = 0; p < PARTICLE_COUNT; p++)
        {
            particlePool[p].Spawn(playerPosition);
        }
        stageStartTime = millis();
        stage = DEAD;
    }
    killTime = millis();
}

// ----------------------------------
// -------- TICKS & RENDERS ---------
// ----------------------------------
void tickStartup(long mm)
{
    FastLED.clear();
    if (stageStartTime + STARTUP_WIPEUP_DUR > mm) // fill to the top with green
    {
        int n = _min(map(((mm - stageStartTime)), 0, STARTUP_WIPEUP_DUR, 0, user_settings.led_count), user_settings.led_count); // fill from top to bottom
        for (int i = 0; i <= n; i++)
        {
            leds[i] = CRGB(0, 255, 0);
        }
    }
    else if (stageStartTime + STARTUP_SPARKLE_DUR > mm) // sparkle the full green bar
    {
        for (int i = 0; i < user_settings.led_count; i++)
        {
            if (random8(30) < 28)
                leds[i] = CRGB(0, 255, 0); // most are green
            else
            {
                int flicker = random8(250);
                leds[i] = CRGB(flicker, 150, flicker); // some flicker brighter
            }
        }
    }
    else if (stageStartTime + STARTUP_FADE_DUR > mm) // fade it out to bottom
    {
        int n = _max(map(((mm - stageStartTime)), STARTUP_SPARKLE_DUR, STARTUP_FADE_DUR, 0, user_settings.led_count), 0); // fill from top to bottom
        int brightness = _max(map(((mm - stageStartTime)), STARTUP_SPARKLE_DUR, STARTUP_FADE_DUR, 255, 0), 0);
        // for(int i = 0; i<= n; i++){

        // leds[i] = CRGB(0, brightness, 0);
        // }
        for (int i = n; i < user_settings.led_count; i++)
        {

            leds[i] = CRGB(0, brightness, 0);
        }
    }
    SFXFreqSweepWarble(STARTUP_FADE_DUR, millis() - stageStartTime, 40, 400, 20);
}

void tickEnemies()
{
    for (int i = 0; i < ENEMY_COUNT; i++)
    {
        if (enemyPool[i].Alive())
        {
            enemyPool[i].Tick();
            // Hit attack?
            if (attacking)
            {
                if (enemyPool[i]._pos > playerPosition - (attack_width / 2) && enemyPool[i]._pos < playerPosition + (attack_width / 2))
                {
                    enemyPool[i].Kill();
                    SFXkill();
                }
            }
            if (inLava(enemyPool[i]._pos))
            {
                enemyPool[i].Kill();
                SFXkill();
            }
            // Draw (if still alive)
            if (enemyPool[i].Alive())
            {
                leds[getLED(enemyPool[i]._pos)] = CRGB(255, 0, 0);
            }
            // Hit player?
            if (
                (enemyPool[i].playerSide == 1 && enemyPool[i]._pos <= playerPosition) ||
                (enemyPool[i].playerSide == -1 && enemyPool[i]._pos >= playerPosition))
            {
                die();
                return;
            }
        }
    }
}

void tickBoss()
{
    // DRAW
    if (boss.Alive())
    {
        boss._ticks++;
        for (int i = getLED(boss._pos - BOSS_WIDTH / 2); i <= getLED(boss._pos + BOSS_WIDTH / 2); i++)
        {
            leds[i] = CRGB::DarkRed;
            leds[i] %= 100;
        }
        // CHECK COLLISION
        if (getLED(playerPosition) > getLED(boss._pos - BOSS_WIDTH / 2) && getLED(playerPosition) < getLED(boss._pos + BOSS_WIDTH))
        {
            die();
            return;
        }
        // CHECK FOR ATTACK
        if (attacking)
        {
            if (
                (getLED(playerPosition + (attack_width / 2)) >= getLED(boss._pos - BOSS_WIDTH / 2) && getLED(playerPosition + (attack_width / 2)) <= getLED(boss._pos + BOSS_WIDTH / 2)) ||
                (getLED(playerPosition - (attack_width / 2)) <= getLED(boss._pos + BOSS_WIDTH / 2) && getLED(playerPosition - (attack_width / 2)) >= getLED(boss._pos - BOSS_WIDTH / 2)))
            {
                boss.Hit();
                if (boss.Alive())
                {
                    moveBoss();
                }
                else
                {
                    spawnPool[0].Kill();
                    spawnPool[1].Kill();
                }
            }
        }
    }
}

void drawPlayer()
{
    leds[getLED(playerPosition)] = CRGB(0, 255, 0);
}

void drawExit()
{
    if (!boss.Alive())
    {
        leds[user_settings.led_count - 1] = CRGB(0, 0, 255);
    }
}

void tickSpawners()
{
    const CRGB defaultCol = CRGB(LAVA_OFF_BRIGHTNESS, LAVA_OFF_BRIGHTNESS / 1.5, 0);
    const CRGB warnCol = CRGB(LAVA_OFF_BRIGHTNESS * 2, LAVA_OFF_BRIGHTNESS * 2, 0);
    unsigned long mm = millis();
    for (int s = 0; s < SPAWN_COUNT; s++)
    {
        if (!spawnPool[s].Alive())
            continue;
        if (mm - spawnPool[s]._lastSpawned > spawnPool[s]._rate + spawnPool[s]._delayOnce)
        {
            spawnEnemy(spawnPool[s]._pos, spawnPool[s]._dir, spawnPool[s]._sp, 0);
            spawnPool[s]._lastSpawned = mm;
            spawnPool[s]._delayOnce = 0;
        }
        long nextSpawn = spawnPool[s]._lastSpawned + spawnPool[s]._rate + spawnPool[s]._delayOnce;
        if (nextSpawn - mm < 800)
        {
            leds[getLED(spawnPool[s]._pos)] = (nextSpawn - mm) % 200 < 100 ? defaultCol : warnCol;
        }
        else
        {
            leds[getLED(spawnPool[s]._pos)] = defaultCol;
        }
    }
}

void tickLava()
{
    int A, B, p, i, brightness, flicker;
    long mm = millis();

    Lava LP;
    for (i = 0; i < LAVA_COUNT; i++)
    {
        LP = lavaPool[i];
        if (LP.Alive())
        {
            LP.Update(); // for grow and flow
            A = getLED(LP._left);
            B = getLED(LP._right);
            if (LP._state == Lava::OFF)
            {
                if (LP._lastOn + LP._offtime < mm)
                {
                    LP._state = Lava::ON;
                    LP._lastOn = mm;
                }
                for (p = A; p <= B; p++)
                {
                    flicker = random8(LAVA_OFF_BRIGHTNESS);
                    leds[p] = CRGB(LAVA_OFF_BRIGHTNESS + flicker, (LAVA_OFF_BRIGHTNESS + flicker) / 1.5, 0);
                }
            }
            else if (LP._state == Lava::ON)
            {
                if (LP._lastOn + LP._ontime < mm)
                {
                    LP._state = Lava::OFF;
                    LP._lastOn = mm;
                }
                for (p = A; p <= B; p++)
                {
                    if (random8(30) < 29)
                        leds[p] = CRGB(150, 0, 0);
                    else
                        leds[p] = CRGB(180, 100, 0);
                }
            }
        }
        lavaPool[i] = LP;
    }
}

bool tickParticles()
{
    bool stillActive = false;
    uint8_t brightness;
    for (int p = 0; p < PARTICLE_COUNT; p++)
    {
        if (particlePool[p].Alive())
        {
            particlePool[p].Tick(USE_GRAVITY);

            if (particlePool[p]._power < 5)
            {
                brightness = (5 - particlePool[p]._power) * 10;
                leds[getLED(particlePool[p]._pos)] += CRGB(brightness, brightness / 2, brightness / 2);
            }
            else
                leds[getLED(particlePool[p]._pos)] += CRGB(particlePool[p]._power, 0, 0);

            stillActive = true;
        }
    }
    return stillActive;
}

void tickConveyors()
{
    // TODO should the visual speed be proportional to the conveyor speed?

    int b, speed, n, i, ss, ee, led;
    long m = 10000 + millis();
    playerPositionModifier = 0;

    int levels = 5; // brightness levels in conveyor

    for (i = 0; i < CONVEYOR_COUNT; i++)
    {
        if (conveyorPool[i]._alive)
        {
            speed = constrain(conveyorPool[i]._speed, -MAX_PLAYER_SPEED + 1, MAX_PLAYER_SPEED - 1);
            ss = getLED(conveyorPool[i]._startPoint);
            ee = getLED(conveyorPool[i]._endPoint);
            for (led = ss; led < ee; led++)
            {

                n = (-led + (m / 100)) % levels;
                if (speed < 0)
                    n = (led + (m / 100)) % levels;

                b = map(n, 5, 0, 0, CONVEYOR_BRIGHTNESS);
                if (b > 0)
                    leds[led] = CRGB(0, 0, b);
            }

            if (playerPosition > conveyorPool[i]._startPoint && playerPosition < conveyorPool[i]._endPoint)
            {
                playerPositionModifier = speed;
            }
        }
    }
}

void tickComplete(long mm) // the boss is dead
{
    int brightness = 0;
    FastLED.clear();
    SFXcomplete();
    if (stageStartTime + 500 > mm)
    {
        int n = _max(map(((mm - stageStartTime)), 0, 500, user_settings.led_count, 0), 0);
        for (int i = user_settings.led_count; i >= n; i--)
        {
            brightness = (sin(((i * 10) + mm) / 500.0) + 1) * 255;
            leds[i].setHSV(brightness, 255, 50);
        }
    }
    else if (stageStartTime + 5000 > mm)
    {
        for (int i = user_settings.led_count; i >= 0; i--)
        {
            brightness = (sin(((i * 10) + mm) / 500.0) + 1) * 255;
            leds[i].setHSV(brightness, 255, 50);
        }
    }
    else if (stageStartTime + 5500 > mm)
    {
        int n = _max(map(((mm - stageStartTime)), 5000, 5500, user_settings.led_count, 0), 0);
        for (int i = 0; i < n; i++)
        {
            brightness = (sin(((i * 10) + mm) / 500.0) + 1) * 255;
            leds[i].setHSV(brightness, 255, 50);
        }
    }
    else
    {
        nextLevel();
    }
}

void tickBossKilled(long mm) // boss funeral
{
    static uint8_t gHue = 0;

    FastLED.setBrightness(constrain(user_settings.led_brightness * 2, MIN_BRIGHTNESS, MAX_BRIGHTNESS)); // super bright!

    int brightness = 0;
    FastLED.clear();

    if (stageStartTime + 6500 > mm)
    {
        gHue++;
        fill_rainbow(leds, user_settings.led_count, gHue, 7); // FastLED's built in rainbow
        if (random8() < 200)
        { // add glitter
            leds[random16(user_settings.led_count)] += CRGB::White;
        }
        SFXbosskilled();
    }
    else if (stageStartTime + 7000 > mm)
    {
        int n = _max(map(((mm - stageStartTime)), 5000, 5500, user_settings.led_count, 0), 0);
        for (int i = 0; i < n; i++)
        {
            brightness = (sin(((i * 10) + mm) / 500.0) + 1) * 255;
            leds[i].setHSV(brightness, 255, 50);
        }
        SFXcomplete();
    }
    else
    {
        FastLED.setBrightness(user_settings.led_brightness);
        nextLevel();
    }
}

void tickDie(long mm)
{                             // a short bright explosion...particles persist after it.
    const int duration = 200; // milliseconds
    const int width = 20;     // half width of the explosion

    if (stageStartTime + duration > mm)
    { // Spread red from player position up and down the width

        int brightness = map((mm - stageStartTime), 0, duration, 255, 150); // this allows a fade from white to red

        // fill up
        int n = _max(map(((mm - stageStartTime)), 0, duration, getLED(playerPosition), getLED(playerPosition) + width), 0);
        for (int i = getLED(playerPosition); i <= n; i++)
        {
            leds[i] = CRGB(255, brightness, brightness);
        }

        // fill to down
        n = _max(map(((mm - stageStartTime)), 0, duration, getLED(playerPosition), getLED(playerPosition) - width), 0);
        for (int i = getLED(playerPosition); i >= n; i--)
        {
            leds[i] = CRGB(255, brightness, brightness);
        }
    }
}

void tickGameover(long mm)
{

    int brightness = 0;

    if (stageStartTime + GAMEOVER_SPREAD_DURATION > mm) // Spread red from player position to top and bottom
    {
        // fill to top
        int n = _max(map(((mm - stageStartTime)), 0, GAMEOVER_SPREAD_DURATION, getLED(playerPosition), user_settings.led_count), 0);
        for (int i = getLED(playerPosition); i <= n; i++)
        {
            leds[i] = CRGB(255, 0, 0);
        }
        // fill to bottom
        n = _max(map(((mm - stageStartTime)), 0, GAMEOVER_SPREAD_DURATION, getLED(playerPosition), 0), 0);
        for (int i = getLED(playerPosition); i >= n; i--)
        {
            leds[i] = CRGB(255, 0, 0);
        }
        SFXgameover();
    }
    else if (stageStartTime + GAMEOVER_FADE_DURATION > mm) // fade down to bottom and fade brightness
    {
        int n = _max(map(((mm - stageStartTime)), GAMEOVER_FADE_DURATION, GAMEOVER_SPREAD_DURATION, user_settings.led_count, 0), 0);
        brightness = map(((mm - stageStartTime)), GAMEOVER_SPREAD_DURATION, GAMEOVER_FADE_DURATION, 200, 0);

        for (int i = 0; i <= n; i++)
        {
            leds[i] = CRGB(brightness, 0, 0);
        }
        SFXcomplete();
    }
}

void tickWin(long mm)
{
    int brightness = 0;
    FastLED.clear();
    if (stageStartTime + WIN_FILL_DURATION > mm)
    {
        int n = _max(map(((mm - stageStartTime)), 0, WIN_FILL_DURATION, user_settings.led_count, 0), 0); // fill from top to bottom
        for (int i = user_settings.led_count; i >= n; i--)
        {
            brightness = user_settings.led_brightness;
            leds[i] = CRGB(0, brightness, 0);
        }
        SFXwin();
    }
    else if (stageStartTime + WIN_CLEAR_DURATION > mm)
    {
        int n = _max(map(((mm - stageStartTime)), WIN_FILL_DURATION, WIN_CLEAR_DURATION, user_settings.led_count, 0), 0); // clear from top to bottom
        for (int i = 0; i < n; i++)
        {
            brightness = user_settings.led_brightness;
            leds[i] = CRGB(0, brightness, 0);
        }
        SFXwin();
    }
    else if (stageStartTime + WIN_OFF_DURATION > mm)
    { // wait a while with leds off
        leds[0] = CRGB(0, user_settings.led_brightness, 0);
    }
    else
    {
        nextLevel();
    }
}

void drawLives()
{
    // show how many lives are left by drawing a short line of green leds for each life
    SFXcomplete(); // stop any sounds
    FastLED.clear();

    int pos = 0;
    for (int i = 0; i < lives; i++)
    {
        for (int j = 0; j < 4; j++)
        {
            leds[pos++] = CRGB(0, 255, 0);
            FastLEDshowESP32();
        }
        leds[pos++] = CRGB(0, 0, 0);
        delay(20);
    }
    FastLEDshowESP32();
    delay(400);
    FastLED.clear();
}

void drawAttack()
{
    if (!attacking)
        return;
    int n = map(millis() - attackMillis, 0, ATTACK_DURATION, 100, 5);
    for (int i = getLED(playerPosition - (attack_width / 2)) + 1; i <= getLED(playerPosition + (attack_width / 2)) - 1; i++)
    {
        leds[i] = CRGB(0, 0, n);
    }
    if (n > 90)
    {
        n = 255;
        leds[getLED(playerPosition)] = CRGB(255, 255, 255);
    }
    else
    {
        n = 0;
        leds[getLED(playerPosition)] = CRGB(0, 255, 0);
    }
    leds[getLED(playerPosition - (attack_width / 2))] = CRGB(n, n, 255);
    leds[getLED(playerPosition + (attack_width / 2))] = CRGB(n, n, 255);
}

int getLED(int pos)
{
    // The world is 1000 pixels wide, this converts world units into an LED number
    return constrain((int)map(pos, 0, VIRTUAL_LED_COUNT, 0, user_settings.led_count - 1), 0, user_settings.led_count - 1);
}

bool inLava(int pos)
{
    // Returns if the player is in active lava
    int i;
    Lava LP;
    for (i = 0; i < LAVA_COUNT; i++)
    {
        LP = lavaPool[i];
        if (LP.Alive() && LP._state == Lava::ON)
        {
            if (LP._left <= pos && LP._right >= pos)
                return true;
        }
    }
    return false;
}

void updateLives()
{
    drawLives();
}

void save_game_stats(bool bossKill)
{
    user_settings.games_played += 1;
    user_settings.total_points += score;
    if (score > user_settings.high_score)
        user_settings.high_score += score;
    if (bossKill)
        user_settings.boss_kills += 1;

    show_game_stats();
    settings_eeprom_write();
}

// ---------------------------------
// --------- SCREENSAVER -----------
// ---------------------------------
void screenSaverTick()
{
    long mm = millis();
    int mode = (mm / 30000) % 7;

    SFXcomplete(); // turn off sound...play testing showed this to be a problem

    switch (mode)
    {
    case 0:
        Fire2012();
        break;
    case 1:
        sinelon();
        break;
    case 2:
        juggle();
        break;
    case 3:
        LED_march();
        break;
    case 4:
        colorWipes();
        break;
    case 5:
        colorWheel();
        break;
    default:
        random_LED_flashes();
    }
}

// ---------------------------------
// ----------- JOYSTICK ------------
// ---------------------------------
bool getInput()
{
    // This is responsible for the player movement speed and attacking.
    // You can replace it with anything you want that passes a -90>+90 value to joystickTilt
    // and any value to joystickWobble that is greater than ATTACK_THRESHOLD (defined at start)
    // For example you could use 3 momentary buttons:
    // if(digitalRead(leftButtonPinNumber) == HIGH) joystickTilt = -90;
    // if(digitalRead(rightButtonPinNumber) == HIGH) joystickTilt = 90;
    // if(digitalRead(attackButtonPinNumber) == HIGH) joystickWobble = ATTACK_THRESHOLD;
    int16_t ax, ay, az;
    int16_t gx, gy, gz;
    ax = ay = az = gx = gy = gz = 0;

    bool connected = accelgyro.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
    if (!connected)
        return false;

    int a = (JOYSTICK_ORIENTATION == 0 ? ax : (JOYSTICK_ORIENTATION == 1 ? ay : az)) / 166;
    int g = (JOYSTICK_ORIENTATION == 0 ? gx : (JOYSTICK_ORIENTATION == 1 ? gy : gz));

#ifdef JOYSTICK_DEBUG
    Serial.print("a:");
    Serial.println(a);
    Serial.print("g:");
    Serial.println(g);
#endif

    if (abs(a) < user_settings.joystick_deadzone)
        a = 0;
    if (a > 0)
        a -= user_settings.joystick_deadzone;
    if (a < 0)
        a += user_settings.joystick_deadzone;
    sample_add(&MPUAngleSamples, a);
    sample_add(&MPUWobbleSamples, g);

    joystickTilt = sample_median(&MPUAngleSamples);
    if (JOYSTICK_DIRECTION == 1)
    {
        joystickTilt = 0 - joystickTilt;
    }
    joystickWobble = abs(sample_highest(&MPUWobbleSamples));

#ifdef JOYSTICK_DEBUG
    // Serial.print("tilt:"); Serial.println(joystickTilt);
    // Serial.print("wobble:"); Serial.println(joystickWobble);
#endif

    return true;
}

// ---------------------------------
// -------------- SFX --------------
// ---------------------------------

/*
   This is used sweep across (up or down) a frequency range for a specified duration.
   A sin based warble is added to the frequency. This function is meant to be called
   on each frame to adjust the frequency in sync with an animation

   duration 	= over what time period is this mapped
   elapsedTime 	= how far into the duration are we in
   freqStart 	= the beginning frequency
   freqEnd 		= the ending frequency
   warble 		= the amount of warble added (0 disables)


*/
void SFXFreqSweepWarble(int duration, int elapsedTime, int freqStart, int freqEnd, int warble)
{
    int freq = map_constrain(elapsedTime, 0, duration, freqStart, freqEnd);
    if (warble)
        warble = map(sin(millis() / 20.0) * 1000.0, -1000, 1000, 0, warble);

    sound(freq + warble, user_settings.audio_volume);
}

/*

   This is used sweep across (up or down) a frequency range for a specified duration.
   Random noise is optionally added to the frequency. This function is meant to be called
   on each frame to adjust the frequency in sync with an animation

   duration 	= over what time period is this mapped
   elapsedTime 	= how far into the duration are we in
   freqStart 	= the beginning frequency
   freqEnd 		= the ending frequency
   noiseFactor 	= the amount of noise to added/subtracted (0 disables)


*/
void SFXFreqSweepNoise(int duration, int elapsedTime, int freqStart, int freqEnd, uint8_t noiseFactor)
{
    int freq;

    if (elapsedTime > duration)
        freq = freqEnd;
    else
        freq = map(elapsedTime, 0, duration, freqStart, freqEnd);

    if (noiseFactor)
        noiseFactor = noiseFactor - random8(noiseFactor / 2);

    sound(freq + noiseFactor, user_settings.audio_volume);
}

void SFXtilt(int amount)
{
    if (amount == 0)
    {
        SFXcomplete();
        return;
    }

    int f = map(abs(amount), 0, 90, 80, 900) + random8(100);
    if (playerPositionModifier < 0)
        f -= 500;
    if (playerPositionModifier > 0)
        f += 200;
    int vol = map(abs(amount), 0, 90, user_settings.audio_volume / 2, user_settings.audio_volume * 3 / 4);
    sound(f, vol);
}
void SFXattacking()
{
    int freq = map(sin(millis() / 2.0) * 1000.0, -1000, 1000, 500, 600);
    if (random8(5) == 0)
    {
        freq *= 3;
    }
    sound(freq, user_settings.audio_volume);
}
void SFXdead()
{
    SFXFreqSweepNoise(1000, millis() - killTime, 1000, 10, 200);
}

void SFXgameover()
{
    SFXFreqSweepWarble(GAMEOVER_SPREAD_DURATION, millis() - killTime, 440, 20, 60);
}

void SFXkill()
{
    sound(2000, user_settings.audio_volume);
}
void SFXwin()
{
    SFXFreqSweepWarble(WIN_OFF_DURATION, millis() - stageStartTime, 40, 400, 20);
}

void SFXbosskilled()
{
    SFXFreqSweepWarble(7000, millis() - stageStartTime, 75, 1100, 60);
}

void SFXcomplete()
{
    soundOff();
}

/*
  This works just like the map function except x is constrained to the range of in_min and in_max
*/
long map_constrain(long x, long in_min, long in_max, long out_min, long out_max)
{
    // constain the x value to be between in_min and in_max
    if (in_max > in_min)
    { // map allows min to be larger than max, but constrain does not
        x = constrain(x, in_min, in_max);
    }
    else
    {
        x = constrain(x, in_max, in_min);
    }
    return map(x, in_min, in_max, out_min, out_max);
}

// Fire2012 by Mark Kriegsman, July 2012
// as part of "Five Elements" shown here: http://youtu.be/knWiGsmgycY
////
// This basic one-dimensional 'fire' simulation works roughly as follows:
// There's a underlying array of 'heat' cells, that model the temperature
// at each point along the line.  Every cycle through the simulation,
// four steps are performed:
//  1) All cells cool down a little bit, losing heat to the air
//  2) The heat from each cell drifts 'up' and diffuses a little
//  3) Sometimes randomly new 'sparks' of heat are added at the bottom
//  4) The heat from each cell is rendered as a color into the leds array
//     The heat-to-color mapping uses a black-body radiation approximation.
//
// Temperature is in arbitrary units from 0 (cold black) to 255 (white hot).
//
// This simulation scales it self a bit depending on NUM_LEDS; it should look
// "OK" on anywhere from 20 to 100 LEDs without too much tweaking.
//
// I recommend running this simulation at anywhere from 30-100 frames per second,
// meaning an interframe delay of about 10-35 milliseconds.
//
// Looks best on a high-density LED setup (60+ pixels/meter).
//
//
// There are two main parameters you can play with to control the look and
// feel of your fire: COOLING (used in step 1 above), and SPARKING (used
// in step 3 above).
//
// COOLING: How much does the air cool as it rises?
// Less cooling = taller flames.  More cooling = shorter flames.
// Default 50, suggested range 20-100
#define COOLING 75

// SPARKING: What chance (out of 255) is there that a new spark will be lit?
// Higher chance = more roaring fire.  Lower chance = more flickery fire.
// Default 120, suggested range 50-200.
#define SPARKING 40

//======================================== SCREEN SAVERS =================

void Fire2012()
{
    // Array of temperature readings at each simulation cell
    static byte heat[VIRTUAL_LED_COUNT]; // the most possible
    bool gReverseDirection = false;

    // Step 1.  Cool down every cell a little
    for (int i = 0; i < user_settings.led_count; i++)
    {
        heat[i] = qsub8(heat[i], random8(0, ((COOLING * 10) / user_settings.led_count) + 2));
    }

    // Step 2.  Heat from each cell drifts 'up' and diffuses a little
    for (int k = user_settings.led_count - 1; k >= 2; k--)
    {
        heat[k] = (heat[k - 1] + heat[k - 2] + heat[k - 2]) / 3;
    }

    // Step 3.  Randomly ignite new 'sparks' of heat near the bottom
    if (random8() < SPARKING)
    {
        int y = random8(7);
        heat[y] = qadd8(heat[y], random8(160, 255));
    }

    // Step 4.  Map from heat cells to LED colors
    for (int j = 0; j < user_settings.led_count; j++)
    {
        CRGB color = HeatColor(heat[j]);
        int pixelnumber;
        if (gReverseDirection)
        {
            pixelnumber = (user_settings.led_count - 1) - j;
        }
        else
        {
            pixelnumber = j;
        }
        leds[pixelnumber] = color;
    }
}

void LED_march()
{
    int n, b, c, i;

    long mm = millis();

    for (i = 0; i < user_settings.led_count; i++)
    {
        leds[i].nscale8(250);
    }

    // Marching green <> orange
    n = (mm / 250) % 10;
    b = 10 + ((sin(mm / 500.00) + 1) * 20.00);
    c = 20 + ((sin(mm / 5000.00) + 1) * 33);
    for (i = 0; i < user_settings.led_count; i++)
    {
        if (i % 10 == n)
        {
            leds[i] = CHSV(c, 255, 150);
        }
    }
}

void random_LED_flashes()
{
    long mm = millis();
    int i;

    for (i = 0; i < user_settings.led_count; i++)
    {
        leds[i].nscale8(250);
    }

    randomSeed(mm);
    for (i = 0; i < user_settings.led_count; i++)
    {
        if (random8(20) == 0)
        {
            leds[i] = CHSV(25, 255, 100);
        }
    }
}

void sinelon()
{
    static uint8_t gHue = 0; // rotating "base color" used by many of the patterns

    gHue++;

    // a colored dot sweeping back and forth, with fading trails
    fadeToBlackBy(leds, user_settings.led_count, 20);
    int pos = beatsin16(13, 0, user_settings.led_count - 1);
    leds[pos] += CHSV(gHue, 255, 192);
}

void juggle()
{
    // eight colored dots, weaving in and out of sync with each other
    fadeToBlackBy(leds, user_settings.led_count, 20);
    byte dothue = 0;
    for (int i = 0; i < 4; i++)
    {
        leds[beatsin16(i + 7, 0, user_settings.led_count - 1)] |= CHSV(dothue, 200, 255);
        dothue += 64;
    }
}

void colorWipes()
{
    // fill led by led with one color after another
    static int mymode = 0;
    switch (mymode)
    {
    case 0:
        if (colorWipe(CRGB(255, 0, 0), 20))
        {
            mymode++;
        }
        break;
    case 1:
        if (colorWipe(CRGB(0, 255, 0), 20))
        {
            mymode++;
        }
        break;
    case 2:
        if (colorWipe(CRGB(0, 0, 255), 20))
        {
            mymode++;
        }
        break;
    case 3:
        if (colorWipe(CRGB(128, 128, 0), 20))
        {
            mymode++;
        }
        break;
    case 4:
        if (colorWipe(CRGB(0, 128, 128), 20))
        {
            mymode++;
        }
        break;
    case 5:
        if (colorWipe(CRGB(128, 0, 128), 20))
        {
            mymode++;
        }
        break;
    default:
        if (colorWipe(CRGB(85, 85, 85), 20))
        {
            mymode = 0;
        }
        break;
    }
}

int colorWipe(CRGB color, int wait)
{
    // fill led by led with one color
    static long rmm = 0;
    long cmm = millis() - rmm;
    uint32_t i = (cmm / wait);

    if (i >= user_settings.led_count)
    {
        rmm = millis();
        cmm = 0;
        if (i == (user_settings.led_count))
        {
            return 1;
        }
    }

    leds[i] = color;
    return 0;
}

void colorWheel()
{
    // cycle hue of each LED with offset between the LEDs
    long mm = millis();
    for (int pos = 0; pos < user_settings.led_count; pos++)
    {
        leds[pos] = CHSV((mm / 10) - (pos * 255 / user_settings.led_count), 255, 128);
    }
}
