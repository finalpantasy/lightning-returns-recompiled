import sys
from spandrel import ModelLoader
for p in sys.argv[1:]:
    m=ModelLoader().load_from_file(p); print(p[-30:],m.architecture.name,m.scale,m.input_channels,m.output_channels,m.size_requirements)
