# Home Assistant and HomeKit Bridge

The ESP32 exposes entities through the ESPHome native API. HomeKit is provided by Home Assistant's HomeKit Bridge integration.

## Main Entities

- Garage-door cover with `device_class: garage`
- Garage light
- Valid HCP broadcast diagnostic
- Error/prewarn/obstruction diagnostics
- Estimated position and clear opening height sensors
- Config numbers for calibrated timing values

## HomeKit

Expose the garage cover through Home Assistant HomeKit Bridge. Include the obstruction binary sensor as a linked obstruction sensor if your Home Assistant version supports that option.

The garage light can be exposed separately. It does not need to be grouped into the garage-door accessory.

## Position Control

HomeKit garage-door accessories do not expose the ESPHome percentage-position model. Use Home Assistant for percentage tests and HomeKit for normal open/close garage-door behavior.

The visual calibration workflow is optional and is only useful for improving Home Assistant percentage targets. It is unnecessary if you only plan to use Apple Home for normal garage-door open/close control.

## If HomeKit Looks Stale

1. Confirm `cover.garage` changes correctly in Home Assistant.
2. Restart the HomeKit Bridge or Home Assistant Core.
3. If only Apple Home is stale, remove and re-add the HomeKit Bridge accessory as a last resort.
