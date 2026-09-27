#include <pawndb/lifecycle.hpp>
#include <pawndb/connection_natives.hpp>
#include <pawndb/mariadb_pool.hpp>
#include <pawndb/postgres_pool.hpp>
#include <sdk.hpp>
#include <Server/Components/Pawn/pawn.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace {

constexpr UID kPawnDbUid(0x5d2ba4ab7ae534f9);
class PawnDbComponent final : public IComponent, public PawnEventHandler, public CoreEventHandler {
 public:
  UID getUID() override { return kPawnDbUid; }
  StringView componentName() const override { return "Pawn.DB"; }
  SemanticVersion componentVersion() const override { return {0, 0, 0, 0}; }

  void onLoad(ICore* core) override {
    core_ = core;
    lifecycle_.start();
    initApi();
    core_->getEventDispatcher().addEventHandler(this);
  }

  void onInit(IComponentList* components) override {
    pawn_ = components->queryComponent<IPawnComponent>();
    if (pawn_) {
      if (!natives_) { lifecycle_.start(); initApi(); }
      pawn_->getEventDispatcher().addEventHandler(this);
    }
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
    natives_.reset();
    connections_.reset();
    scripts_.clear();
    lifecycle_.stop();
  }

  void free() override {
    if (pawn_) pawn_->getEventDispatcher().removeEventHandler(this);
    if (core_) core_->getEventDispatcher().removeEventHandler(this);
    natives_.reset();
    connections_.reset();
    scripts_.clear();
    lifecycle_.stop();
    delete this;
  }

  void reset() override {
    natives_.reset();
    connections_.reset();
    scripts_.clear();
    lifecycle_.stop();
    lifecycle_.start();
    initApi();
  }

  void onTick(Microseconds, TimePoint) override { lifecycle_.dispatch_tick(); }
  void onAmxLoad(IPawnScript& script) override {
    if (scripts_.contains(script.GetAMX())) return;
    if (natives_ && script.Register(pawndb::ConnectionNatives::table(), 12) == AMX_ERR_NONE) {
      lifecycle_.attach(script.GetAMX());
      scripts_[script.GetAMX()] = &script;
    }
  }
  void onAmxUnload(IPawnScript& script) override {
    if (connections_) connections_->detach(script.GetAMX());
    lifecycle_.detach(script.GetAMX());
    scripts_.erase(script.GetAMX());
  }

 private:
  void initApi() {
    connections_ = std::make_unique<pawndb::ConnectionManager>(
        lifecycle_, [this](void* amx, auto handle, int code, std::string message) {
          onConnectionError(static_cast<AMX*>(amx), handle, code, message);
        }, [this](std::string message) {
          core_->logLn(LogLevel::Warning, "%s", message.c_str());
        }, [](const pawndb::ConnectionConfig& config, pawndb::DriverError& error) {
          return config.backend == pawndb::Backend::postgres
                     ? pawndb::PostgresPool::open(config, error)
                     : pawndb::MariaPool::open(config, error);
        });
    natives_ = std::make_unique<pawndb::ConnectionNatives>(
        *connections_,
        [this](AMX* amx, cell address, std::string& value) {
          auto it = scripts_.find(amx);
          if (it == scripts_.end()) return false;
          cell* source = nullptr;
          int length = 0;
          if (it->second->GetAddr(address, &source) != AMX_ERR_NONE ||
              it->second->StrLen(source, &length) != AMX_ERR_NONE || length < 0) return false;
          value.resize(static_cast<std::size_t>(length) + 1);
          if (it->second->GetString(value.data(), source, false, value.size()) != AMX_ERR_NONE)
            return false;
          value.resize(length);
          return true;
        },
        [this](AMX* amx, cell address, std::string_view value, std::size_t capacity) {
          auto it = scripts_.find(amx);
          if (it == scripts_.end() || !capacity) return false;
          cell* output = nullptr;
          if (it->second->GetAddr(address, &output) != AMX_ERR_NONE) return false;
          const auto truncated = value.substr(0, capacity - 1);
          return it->second->SetString(output, StringView(truncated.data(), truncated.size()),
                                       false, false, capacity) == AMX_ERR_NONE;
        });
  }

  void onConnectionError(AMX* amx, pawndb::ConnectionManager::Handle handle,
                         int code, const std::string& message) {
    auto it = scripts_.find(amx);
    if (it == scripts_.end()) return;
    int index = 0;
    if (it->second->FindPublic("OnConnectionError", &index) != AMX_ERR_NONE) return;
    cell address = 0, ret = 0;
    if (it->second->PushString(&address, nullptr, message, false, false) != AMX_ERR_NONE)
      return;
    if (it->second->Push(code) == AMX_ERR_NONE &&
        it->second->Push(static_cast<cell>(handle)) == AMX_ERR_NONE)
      it->second->Exec(&ret, index);
    it->second->Release(address);
  }

  ICore* core_ = nullptr;
  IPawnComponent* pawn_ = nullptr;
  pawndb::Lifecycle lifecycle_;
  std::unique_ptr<pawndb::ConnectionManager> connections_;
  std::unique_ptr<pawndb::ConnectionNatives> natives_;
  std::unordered_map<AMX*, IPawnScript*> scripts_;
};

}  // namespace

COMPONENT_ENTRY_POINT() { return new PawnDbComponent; }
