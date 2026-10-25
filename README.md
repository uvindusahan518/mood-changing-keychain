# mood-changing-keychain 🎭
A tiny DIY electronic keytag that brings a little personality to everyday accessories.

Built using an ESP32-C3 and a small OLED display, this project displays different facial expressions that can be changed with a button press.

✨ Features

Displays different expressions on a tiny OLED screen

Changes moods using a push button

Includes a physical on/off switch

Compact design made to hang on a bag

Rechargeable battery-powered design

🧰 Components Used

ESP32-C3 development board

Small OLED display

3.7 V LiPo battery

LiPo charging circuit

Push button for changing expressions

On/off switch

Perf board board

Connecting wires and solder

Key ring

⚙️ How It Works

The ESP32-C3 controls the OLED display and changes the displayed expression when the button is pressed.

💻 Software

The project code is written for the Arduino development environment.

Install the Arduino IDE.

Install the board support package required for your ESP32-C3 board.

Install the display libraries required by the code.

Open mood_changing_keytag.ino.

Select the correct board and port.

Upload the code to the ESP32-C3.

Check the #include lines in the sketch to identify the exact libraries needed.

🔌 Circuit Connections

Add the wiring diagram and the pin connections used in your final build here.

Component

Connection

OLED VCC

Actual power pin used

OLED GND

GND

OLED SDA

Actual SDA GPIO

OLED SCL

Actual SCL GPIO

Push button

Configured GPIO and GND, if using INPUT_PULLUP

On/off switch

Actual power-switch wiring

Replace the descriptions in the table with your actual connections before publishing.

🛠️ Build Process

Gather the electronic components.

Test the circuit on a breadboard.

Upload and test the code.

Arrange the components on a small dot board.

Solder the connections.

Add the on/off switch.

Attach the key ring.

Test the completed keytag.

🤖 AI Assistance

Claude AI assisted me with the code during development. I used the code as part of building and testing the physical project.

🔋 Battery Safety

Use a charging circuit compatible with the LiPo battery's specifications. Verify the charging current, battery connections, and power requirements before operating or charging the completed device.

📸 Project Photos

Photos, wiring diagrams, and a demonstration video can be added to this repository to document the build.

Made as a personal DIY electronics project. One little idea, one little creation.
