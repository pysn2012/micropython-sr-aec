# This file is executed on every boot (including wake-boot from deepsleep)
#import esp
#esp.osdebug(None)
#import webrepl
#webrepl.start()

from test_logic import SensorSystem

def main():
    system = SensorSystem()
    system.run()

if __name__ == "__main__":
    main()
