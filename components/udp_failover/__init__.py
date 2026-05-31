import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PORT
import logging

CODEOWNERS = ["@dimkalinux"]
DEPENDENCIES = ["network"]
AUTO_LOAD = ["socket"]

logging.info("Load 'udp_failover' component from https://github.com/dimkalinux/esphome-components")
logging.info("If you like the 'udp_failover' component, you can support it with a star ⭐ on GitHub.")

CONF_GROUP_ID = "group_id"
CONF_MULTICAST_ADDRESS = "multicast_address"

udp_failover_ns = cg.esphome_ns.namespace("udp_failover")
UdpFailoverComponent = udp_failover_ns.class_("UdpFailoverComponent", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(UdpFailoverComponent),
    cv.Required(CONF_GROUP_ID): cv.All(cv.string, cv.Length(min=2, max=8)),
    cv.Optional(CONF_MULTICAST_ADDRESS, default="239.255.84.79"): cv.ipv4address,
    cv.Optional(CONF_PORT, default=14479): cv.port,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_group_id(config[CONF_GROUP_ID]))
    cg.add(var.set_multicast_address(str(config[CONF_MULTICAST_ADDRESS])))
    cg.add(var.set_port(config[CONF_PORT]))
