#version 460

vec2 ducky[3] = vec2[] (
                        vec2( 0.0     ,    0.5), // flipped y, so is P_0(0,-0.5)
                        vec2(-0.5     ,   -0.5),
                        vec2( 0.5     ,   -0.5)
                        );

void main(){
  gl_Position = vec4(ducky[gl_VertexIndex],0.0, 1.0);
}
