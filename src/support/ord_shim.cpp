// eda-lab embeds only odb + utl from OpenROAD, not the application layer.
// Some utl.a members (the swig/Tcl wrappers, LoggerCommon) reference the
// application's globals and get dragged into a link whenever the linker
// resolves any stray symbol from those objects. Provide inert definitions
// so such pulls always link; nothing in eda-lab calls these paths.

namespace utl {
class Logger;
}

namespace ord {

class OpenRoad
{
 public:
  static OpenRoad* openRoad();
};

utl::Logger* getLogger();

OpenRoad* OpenRoad::openRoad()
{
  return nullptr;
}

utl::Logger* getLogger()
{
  return nullptr;
}

}  // namespace ord
