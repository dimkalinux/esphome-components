import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
import logging

CODEOWNERS = ["@dimkalinux"]

logging.info("Load 'espnow_failover' component from https://github.com/dimkalinux/esphome-components")
logging.info("If you like the 'espnow_failover' component, you can support it with a star ⭐ on GitHub.")

CONF_GROUP_ID = "group_id"

espnow_failover_ns = cg.esphome_ns.namespace("espnow_failover")
EspNowFailoverComponent = espnow_failover_ns.class_("EspNowFailoverComponent", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(EspNowFailoverComponent),
    cv.Required(CONF_GROUP_ID): cv.All(cv.string, cv.Length(min=2, max=8)),
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_group_id(config[CONF_GROUP_ID]))
