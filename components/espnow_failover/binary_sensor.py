import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CONNECTIVITY,
)
from . import EspNowFailoverComponent, espnow_failover_ns

CONF_ESPNOW_FAILOVER_ID = "espnow_failover_id"
CONF_IS_MASTER = "is_master"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ESPNOW_FAILOVER_ID): cv.use_id(EspNowFailoverComponent),
        cv.Optional(CONF_IS_MASTER): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_ESPNOW_FAILOVER_ID])

    if is_master_config := config.get(CONF_IS_MASTER):
        sens = await binary_sensor.new_binary_sensor(is_master_config)
        cg.add(parent.set_is_master_binary_sensor(sens))
