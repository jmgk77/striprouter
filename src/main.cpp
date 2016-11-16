// Style: http://geosoft.no/development/cppstyle.html

#include <cstdio>
#include <ctime>
#include <iostream>
#include <mutex>
#include <thread>

#include <boost/filesystem.hpp>
#include <fmt/format.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <nanogui/nanogui.h>

#include "dijkstra.h"
#include "gl_error.h"
#include "ogl_text.h"
#include "pcb.h"
#include "utils.h"


using namespace std;
using namespace boost::filesystem;
using namespace nanogui;


const u32 windowW = 1920 / 2;
const u32 windowH = 1080 - 50;
const char *FONT_PATH = "./fonts/LiberationMono-Regular.ttf";
const u32 FONT_SIZE = 18;

Costs costs;
Circuit circuit;
void parseAndRoute();
void runParseAndAutoroute();
thread parseAndAutorouteThread;
OglText* oglTextPtr;
PcbDraw* pcbDrawPtr;
averageSec averageRendering;
std::time_t mtime_prev = 0;


class Application : public nanogui::Screen
{
public:
  Application()
    : nanogui::Screen(Eigen::Vector2i(windowW, windowH), "Stripboard Autorouter", true, false, 8, 8, 24, 8, 4, 3, 3)
  {
    GLuint mTextureId;
    glGenTextures(1, &mTextureId);

    glfwMakeContextCurrent(glfwWindow());

    // Initialize GLEW.
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
      fprintf(stderr, "Failed to initialize GLEW\n");
      glfwTerminate();
      return;
    }

    glGetError();
    glClearColor(0.3f, 0.3f, 0.3f, 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    glGenVertexArrays(1, &vertexArrayId);

    glBindVertexArray(vertexArrayId);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, windowW, windowH);

    oglTextPtr = new OglText(windowW, windowH, FONT_PATH, FONT_SIZE);
    pcbDrawPtr = new PcbDraw(windowW, windowH);

    // Main grid window
    auto window = new Window(this, "Router");
    window->setPosition(Vector2i(windowW - 400, windowH - 300));
    GridLayout *layout = new GridLayout(Orientation::Horizontal, 2, Alignment::Middle, 15, 5);
    layout->setColAlignment({Alignment::Maximum, Alignment::Fill});
    layout->setSpacing(0, 10);
    window->setLayout(layout);

//    mProgress = new ProgressBar(window);

    {
      new Label(window, "Wire cost:", "sans-bold");
      auto intBox = new IntBox<int>(window);
      intBox->setEditable(true);
      intBox->setFixedSize(Vector2i(100, 20));
      intBox->setValue(costs.wire);
      intBox->setDefaultValue("0");
      intBox->setFontSize(18);
      intBox->setFormat("[1-9][0-9]*");
      intBox->setSpinnable(true);
      intBox->setMinValue(1);
      intBox->setValueIncrement(1);
      intBox->setCallback([intBox](int value) {
        costs.wire = value;
        runParseAndAutoroute();
      });
    }
    {
      new Label(window, "Strip cost:", "sans-bold");
      auto intBox = new IntBox<int>(window);
      intBox->setEditable(true);
      intBox->setFixedSize(Vector2i(100, 20));
      intBox->setValue(costs.strip);
      intBox->setDefaultValue("0");
      intBox->setFontSize(18);
      intBox->setFormat("[1-9][0-9]*");
      intBox->setSpinnable(true);
      intBox->setMinValue(1);
      intBox->setValueIncrement(1);
      intBox->setCallback([intBox](int value) {
        costs.strip = value;
        runParseAndAutoroute();
      });
    }
    {
      new Label(window, "Via cost:", "sans-bold");
      auto intBox = new IntBox<int>(window);
      intBox->setEditable(true);
      intBox->setFixedSize(Vector2i(100, 20));
      intBox->setValue(costs.via);
      intBox->setDefaultValue("0");
      intBox->setFontSize(18);
      intBox->setFormat("[1-9][0-9]*");
      intBox->setSpinnable(true);
      intBox->setMinValue(1);
      intBox->setValueIncrement(1);
      intBox->setCallback([intBox](int value) {
        costs.via = value;
        runParseAndAutoroute();
      });
    }
    {
      new Label(window, "Show connections:", "sans-bold");
      auto cb = new CheckBox(window, "");
      cb->setFontSize(18);
      cb->setChecked(false);
    }
    {
      new Label(window, "Zoom:", "sans-bold");

      Widget *panel = new Widget(window);
      panel->setLayout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 5));

      Slider *slider = new Slider(panel);
      slider->setValue(0.25f);
      slider->setFixedWidth(100);

      TextBox *textBox = new TextBox(panel);
      textBox->setFixedSize(Vector2i(50, 20));
      textBox->setValue("50");
      textBox->setUnits("%");
      slider->setCallback([textBox](float value) {
        textBox->setValue(std::to_string((int) (value * 200)));
        pcbDrawPtr->setZoom(value * 4.0f);
      });
      slider->setFinalCallback([&](float value) {
        pcbDrawPtr->setZoom(value * 4.0f);
      });
      textBox->setFixedSize(Vector2i(60,25));
      textBox->setFontSize(18);
      textBox->setAlignment(TextBox::Alignment::Right);
    }
    performLayout();
  }


  ~Application()
  {
    delete oglTextPtr;
    delete pcbDrawPtr;
  }


  virtual bool keyboardEvent(int key, int scancode, int action, int modifiers)
  {
    if (Screen::keyboardEvent(key, scancode, action, modifiers)) {
      return true;
    }
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
      setVisible(false);
      return true;
    }
    return false;
  }


  virtual void draw(NVGcontext *ctx)
  {
    // Draw the user interface
    Screen::draw(ctx);
  }


  virtual void drawContents()
  {
    glBindVertexArray(vertexArrayId);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, windowW, windowH);

    std::time_t mtime_cur = boost::filesystem::last_write_time("./circuit.txt");
    if (mtime_cur != mtime_prev) {
      mtime_prev = mtime_cur;
      runParseAndAutoroute();
    }

    glfwSetTime(0.0);

    pcbDrawPtr->draw(circuit);

    // Needed for timing but can be very bad for performance.
    glFinish();

    averageRendering.addSec(glfwGetTime());
    glfwSetTime(0.0);

    auto avgRenderingSec = averageRendering.calcAverage();

    auto nLine = 0;
    oglTextPtr->print(0, 0, nLine++, fmt::format("Render: {:.1f}ms", avgRenderingSec * 1000));
    for (auto s : circuit.getCircuitInfoVec()) {
      oglTextPtr->print(0, 0, nLine++, s);
    }
  }

private:
  nanogui::ProgressBar *mProgress;
  GLuint vertexArrayId;
};


int main(int argc, char **argv)
{
  try {
    nanogui::init();
    {
      nanogui::ref<Application> app = new Application();
      app->drawAll();
      app->setVisible(true);
      nanogui::mainloop();
    }
    nanogui::shutdown();
  }
  catch (const std::runtime_error &e) {
    std::string error_msg = std::string("Caught a fatal error: ") + std::string(e.what());
#if defined(_WIN32)
	//MessageBoxA(nullptr, error_msg.c_str(), NULL, MB_ICONERROR | MB_OK);
#else
    std::cerr << error_msg << endl;
#endif
    return -1;
  }
  return 0;
}


void parseAndRoute()
{
  auto parser = Parser();
  {
    lock_guard<mutex> lockCircuit(circuitMutex);
    circuit = parser.parse();
  }
  if (!circuit.getErrorBool()) {
    Dijkstra dijkstra;
    // Block the component footprints from being used by routes.
    for (auto componentName : circuit.getComponentNameVec()) {
      auto ci = circuit.getComponentInfoMap().find(componentName)->second;
      for (int y = ci.footprint.start.y; y <= ci.footprint.end.y; ++y) {
        for (int x = ci.footprint.start.x; x <= ci.footprint.end.x; ++x) {
          HoleCoord wireSideCoord(x, y, true);
          HoleCoord stripSideCoord(x, y, false);
          dijkstra.addCoordToUsed(wireSideCoord);
          dijkstra.addCoordToUsed(stripSideCoord);
        }
      }
    }
    for (auto startEndCoord : circuit.getConnectionCoordVec()) {
      dijkstra.findCheapestRoute(costs, circuit, startEndCoord);
      if (circuit.getErrorBool()) {
        break;
      }
    }
  }
  if (!circuit.getErrorBool()) {
    circuit.getCircuitInfoVec().push_back(fmt::format("Ok"));
  }
}


void runParseAndAutoroute()
{
  circuit.setErrorBool(true);
  try {
    parseAndAutorouteThread.join();
  }
  catch (const std::system_error &e) {
  }
  parseAndAutorouteThread = thread(parseAndRoute);
}