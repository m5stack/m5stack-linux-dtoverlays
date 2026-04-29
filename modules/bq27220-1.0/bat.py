import smbus
import time

# --- Configuration ---
# BQ27220 Default I2C Address
DEVICE_ADDRESS = 0x55

# Raspberry Pi I2C Bus ID (usually 1 for modern Pis)
I2C_BUS_ID = 1

# --- BQ27220 Register Map (Standard Commands) ---
# Units: 0.1 deg K
REG_TEMPERATURE = 0x06 
# Units: mV
REG_VOLTAGE = 0x08     
# Units: mA (Signed Integer). Note: 0x14 is AverageCurrent, 0x0C is Current
REG_AVG_CURRENT = 0x14 

def get_signed_16bit(data):
    """
    Converts an unsigned 16-bit integer (0-65535) from I2C
    to a signed Python integer.
    Required for Current readings (charging is positive, discharging is negative).
    """
    if data > 32767:
        return data - 65536
    return data

def main():
    try:
        # Initialize I2C bus
        bus = smbus.SMBus(I2C_BUS_ID)
        print("I2C Bus Initialized. Reading BQ27220...")
        print("Press CTRL+C to stop.")
        print("-" * 40)

        while True:
            try:
                # 1. Read Voltage
                # Data comes in Little Endian, read_word_data handles this automatically
                raw_volt = bus.read_word_data(DEVICE_ADDRESS, REG_VOLTAGE)
                voltage_mv = raw_volt # Unit is already mV

                # 2. Read Average Current
                raw_current = bus.read_word_data(DEVICE_ADDRESS, REG_AVG_CURRENT)
                # Convert to signed integer (e.g., -500 mA)
                avg_current_ma = get_signed_16bit(raw_current)

                # 3. Read Temperature
                raw_temp = bus.read_word_data(DEVICE_ADDRESS, REG_TEMPERATURE)
                # Convert 0.1 Kelvin to Celsius
                # Formula: (0.1 * K) - 273.15
                temp_kelvin = raw_temp * 0.1
                temp_celsius = temp_kelvin - 273.15

                # Output Data
                print(f"Voltage     : {voltage_mv} mV")
                print(f"Avg Current : {avg_current_ma} mA")
                print(f"Temperature : {temp_celsius:.2f} C")
                print("-" * 40)

            except OSError:
                print("Error: I2C Read Failed. Check wiring.")
            
            # Delay before next read
            time.sleep(2)

    except KeyboardInterrupt:
        print("\nStopped by user.")
    except Exception as e:
        print(f"System Error: {e}")

if __name__ == "__main__":
    main()