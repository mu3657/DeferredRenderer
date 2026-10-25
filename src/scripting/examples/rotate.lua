return {
    properties = {
        speed_degrees = { type = "number", default = 45, min = -360, max = 360 }
    },
    on_update = function(self, dt)
        self.node.transform:rotate_local_y_radians(math.rad(self.properties.speed_degrees) * dt)
    end
}
