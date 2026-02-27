import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    STATE_CLASS_MEASUREMENT,
    ICON_COUNTER,
)
from . import EspNowFailoverComponent, espnow_failover_ns

CONF_ESPNOW_FAILOVER_ID = "espnow_failover_id"
CONF_PEER_COUNT = "peer_count"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ESPNOW_FAILOVER_ID): cv.use_id(EspNowFailoverComponent),
        cv.Required(CONF_PEER_COUNT): sensor.sensor_schema(
            icon=ICON_COUNTER,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_ESPNOW_FAILOVER_ID])

    if peer_count_config := config.get(CONF_PEER_COUNT):
        sens = await sensor.new_sensor(peer_count_config)
        cg.add(parent.set_peer_count_sensor(sens))

