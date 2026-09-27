#include <pawndb/lifecycle.hpp>
#include <sdk.hpp>
#include <Server/Components/Pawn/pawn.hpp>

namespace {

constexpr UID kPawnDbUid(0x5d2ba4ab7ae534f9);
constexpr AMX_NATIVE_INFO kNatives[] = {{nullptr, nullptr}};

class PawnDbComponent final : public IComponent, public PawnEventHandler, public CoreEventHandler {
 public:
  UID getUID() override { return kPawnDbUid; }
  StringView componentName() const override { return "Pawn.DB"; }
  SemanticVersion componentVersion() const override { return {0, 0, 0, 0}; }

  void onLoad(ICore* core) override {
    core_ = core;
    lifecycle_.start();
    core_->getEventDispatcher().addEventHandler(this);
  }

  void onInit(IComponentList* components) override {
    pawn_ = components->queryComponent<IPawnComponent>();
    if (pawn_) pawn_->getEventDispatcher().addEventHandler(this);
  }

  void onReady() override {
    if (!pawn_) return;
    if (auto* script = pawn_->mainScript()) onAmxLoad(*script);
    for (auto* script : pawn_->sideScripts()) onAmxLoad(*script);
  }

  void onFree(IComponent* component) override {
    if (component != pawn_) return;
    pawn_->getEventDispatcher().removeEventHandler(this);
    pawn_ = nullptr;
    lifecycle_.stop();
  }

  void free() override {
    if (pawn_) pawn_->getEventDispatcher().removeEventHandler(this);
    if (core_) core_->getEventDispatcher().removeEventHandler(this);
    lifecycle_.stop();
    delete this;
  }

  void reset() override {
    lifecycle_.stop();
    lifecycle_.start();
  }

  void onTick(Microseconds, TimePoint) override { lifecycle_.dispatch_tick(); }
  void onAmxLoad(IPawnScript& script) override {
    if (script.Register(kNatives, 0) == AMX_ERR_NONE) lifecycle_.attach(script.GetAMX());
  }
  void onAmxUnload(IPawnScript& script) override { lifecycle_.detach(script.GetAMX()); }

 private:
  ICore* core_ = nullptr;
  IPawnComponent* pawn_ = nullptr;
  pawndb::Lifecycle lifecycle_;
};

}  // namespace

COMPONENT_ENTRY_POINT() { return new PawnDbComponent; }
