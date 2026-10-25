return {
    properties = {
        minimum = { type = "number", default = 5.0, min = 0.0, max = 1000.0 },
        maximum = { type = "number", default = 35.0, min = 0.0, max = 1000.0 },
        speed = { type = "number", default = 1.0, min = 0.0, max = 10.0 }
    },
    on_create = function(self)
        self.state.time = 0
    end,
    on_update = function(self, dt)
        self.state.time = self.state.time + dt
        local blend = 0.5 + 0.5 * math.sin(self.state.time * self.properties.speed)
        self.node.light:set_intensity(self.properties.minimum + (self.properties.maximum - self.properties.minimum) * blend)
    end
}
