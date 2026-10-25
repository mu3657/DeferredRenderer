-- Each instance gets its own globals, properties and self.state.
return {
    properties = {
        name = { type = "string", default = "timer" },
        speed = { type = "number", default = 1.0, min = 0.0, max = 10.0 },
        enabled = { type = "bool", default = true },
        offset = { type = "vec3", default = { x = 0, y = 0, z = 0 } }
    },

    on_create = function(self)
        self.state.elapsed = 0
        log(self.properties.name .. " created")
    end,

    on_update = function(self, dt)
        if not self.properties.enabled then
            return
        end
        self.state.elapsed = self.state.elapsed + dt * self.properties.speed
        log(string.format("%s elapsed=%.2f", self.properties.name, self.state.elapsed))
    end,

    on_destroy = function(self)
        log(self.properties.name .. " destroyed")
    end
}
