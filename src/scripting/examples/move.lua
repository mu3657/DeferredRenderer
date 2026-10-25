return {
    properties = {
        amplitude = { type = "number", default = 1.0, min = 0.0, max = 10.0 },
        speed = { type = "number", default = 1.0, min = 0.0, max = 10.0 },
        axis = { type = "vec3", default = { x = 1, y = 0, z = 0 } }
    },
    on_create = function(self)
        self.state.origin = self.node.transform:get_local_position()
        self.state.time = 0
    end,
    on_update = function(self, dt)
        self.state.time = self.state.time + dt
        local distance = math.sin(self.state.time * self.properties.speed) * self.properties.amplitude
        local origin = self.state.origin
        local axis = self.properties.axis
        self.node.transform:set_local_position({
            x = origin.x + axis.x * distance,
            y = origin.y + axis.y * distance,
            z = origin.z + axis.z * distance
        })
    end
}
