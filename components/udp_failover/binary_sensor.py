import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    DEVICE_CLASS_CONNECTIVITY,
)
from . import UdpFailoverComponent

CONF_UDP_FAILOVER_ID = "udp_failover_id"
CONF_IS_MASTER = "is_master"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_UDP_FAILOVER_ID): cv.use_id(UdpFailoverComponent),
        cv.Optional(CONF_IS_MASTER): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_UDP_FAILOVER_ID])

    if is_master_config := config.get(CONF_IS_MASTER):
        sens = await binary_sensor.new_binary_sensor(is_master_config)
        cg.add(parent.set_is_master_binary_sensor(sens))
