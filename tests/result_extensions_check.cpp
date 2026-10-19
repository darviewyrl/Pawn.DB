#include <pawndb/connection_natives.hpp>

#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <unordered_map>

#define CHECK(x) do { if (!(x)) { std::cerr << "result extension check failed at " << __LINE__ << '\n'; return 1; } } while (false)

int main() {
  using namespace pawndb;
  Lifecycle life; life.start();
  AMX amx{};
  std::array<cell, 512> memory{};
  amx.data = reinterpret_cast<unsigned char*>(memory.data());
  amx.hea = 256 * sizeof(cell); amx.stk = 300 * sizeof(cell); amx.stp = sizeof(memory);
  CHECK(life.attach(&amx));
  int warnings = 0;
  std::unordered_map<cell, std::string> input = {
      {101,"doc"},{102,"vec"},{103,"vec4"},{104,"bad"},{105,"nil"},{106,"missing"},
      {107,"nan"},{108,"inf"},{109,"space"},
      {201,"$.inventory.slots[2].attributes.durability"},{202,"$.truth"},
      {203,"$.id"},{204,"$.items"},{205,"$.object"},{206,"$.integral"},
      {207,"$.vector"},{208,"$.vector4"},{209,"$.inventory[*]"},
      {210,"$.missing"},{211,"$.text"},{212,"$['key.name']"},{213,"$.vector_bad"},{214,"$[\"key.name\"]"}};
  std::unordered_map<cell, std::string> output;
  ConnectionManager manager(life, [](void*, auto, int, std::string) {}, [&](std::string) { ++warnings; });
  ConnectionNatives natives(manager,
      [&](AMX*, cell address, std::string& text) { auto it=input.find(address); if (it==input.end()) return false; text=it->second; return true; },
      [&](AMX*, cell address, std::string_view text, std::size_t capacity) { output[address]=text.substr(0,capacity-1); return true; },
      [] { return false; });
  auto native = [&](const char* name) {
    for (auto* item=ConnectionNatives::table(); item->name; ++item) if (!std::strcmp(item->name,name)) return item->func;
    return static_cast<AMX_NATIVE>(nullptr);
  };
  CHECK(ConnectionNatives::native_count == 75);
  QueryResult data;
  data.fields={"doc","vec","vec4","bad","nil","nan","inf","space"};
  data.rows={{R"({"inventory":{"slots":[{"attributes":{"durability":1}},{"attributes":{"durability":2}},{"attributes":{"durability":12}}]},"truth":true,"id":18446744073709551615,"items":[1,"x"],"object":{"ok":true},"integral":7.0,"vector":[1,-2.5,3],"vector4":[4,5,6,7],"vector_bad":[1e39,2,3],"text":"hello","key.name":9})",
              "1532.125,-1688.5432,13.5","1,2,3,4","1,2,3x",std::nullopt,"NaN,2,3","Inf,2,3","1, 2,3"}};
  const auto handle=manager.create_result(&amx,std::move(data)); CHECK(handle);
  const cell h=static_cast<cell>(handle);
  auto call=[&](const char* name,std::initializer_list<cell> args) { std::vector<cell> p{static_cast<cell>(args.size()*sizeof(cell))}; p.insert(p.end(),args); return native(name)(&amx,p.data()); };
  CHECK(call("pdb_activate_result",{h}));
  CHECK(call("pdb_json_get_int",{101,201,-1})==12);
  CHECK(call("pdb_json_get_int",{101,206,-1})==7);
  CHECK(call("pdb_json_get_bool",{101,202,-1})==1);
  CHECK(call("pdb_json_get_bool",{101,206,-1})==0);
  CHECK(std::bit_cast<float>(call("pdb_json_get_float",{101,206,-1}))==7.0f);
  CHECK(call("pdb_json_get_str",{101,203,700,64,-1})==1 && output[700]=="18446744073709551615");
  CHECK(call("pdb_json_get_str",{101,204,701,64,-1})==1 && output[701]=="[1,\"x\"]");
  CHECK(call("pdb_json_get_str",{101,211,702,5,-1})==0 && output[702]=="hell");
  CHECK(call("pdb_json_get_str",{101,206,703,16,-1})==0 && output[703].empty());
  CHECK(call("pdb_json_get_str",{101,212,704,16,-1})==1 && output[704]=="9");
  CHECK(call("pdb_json_get_str",{101,214,705,16,-1})==1 && output[705]=="9");
  const int before_overflow=warnings;
  CHECK(call("pdb_json_get_int",{101,203,-1})==0 && warnings>before_overflow);
  CHECK(call("pdb_json_get_int",{101,209,-1})==0 && call("pdb_json_get_int",{101,210,-1})==0);
  CHECK(call("pdb_json_get_int",{104,201,-1})==0 && call("pdb_json_get_int",{101,201,-1})==12);
  CHECK(call("pdb_json_get_int",{105,201,-1})==0 && call("pdb_json_get_int",{106,201,-1})==0);

  constexpr cell x=80*sizeof(cell), y=81*sizeof(cell), z=82*sizeof(cell), w=83*sizeof(cell);
  auto f=[&](cell address) { return std::bit_cast<float>(memory[static_cast<std::size_t>(address/sizeof(cell))]); };
  memory[80]=memory[81]=memory[82]=memory[83]=0;
  CHECK(call("pdb_json_get_vector",{101,207,x,y,z,-1}) && f(x)==1.0f && f(y)==-2.5f && f(z)==3.0f);
  CHECK(call("pdb_json_get_vector4",{101,208,x,y,z,w,-1}) && f(w)==7.0f);
  CHECK(!call("pdb_json_get_vector",{101,213,x,y,z,-1}) && f(x)==0.0f && f(y)==0.0f && f(z)==0.0f);
  CHECK(!call("pdb_json_get_vector",{101,208,x,y,z,-1}) && f(x)==0.0f && f(y)==0.0f && f(z)==0.0f);
  CHECK(call("pdb_get_vector",{102,x,y,z,-1}) && std::abs(f(x)-1532.125f)<1e-4f && std::abs(f(y)+1688.5432f)<1e-4f);
  CHECK(call("pdb_get_vector_by_index",{1,x,y,z,-1}) && f(z)==13.5f);
  CHECK(call("pdb_get_vector4",{103,x,y,z,w,-1}) && f(w)==4.0f);
  CHECK(call("pdb_get_vector4_by_index",{2,x,y,z,w,-1}) && f(w)==4.0f);
  CHECK(!call("pdb_get_vector",{103,x,y,z,-1}) && f(x)==0.0f && f(y)==0.0f && f(z)==0.0f);
  CHECK(!call("pdb_get_vector4",{102,x,y,z,w,-1}) && f(x)==0.0f && f(w)==0.0f);
  CHECK(!call("pdb_get_vector",{104,x,y,z,-1}) && f(x)==0.0f);
  CHECK(!call("pdb_get_vector",{105,x,y,z,-1}) && f(x)==0.0f);
  CHECK(!call("pdb_get_vector",{107,x,y,z,-1}) && f(x)==0.0f);
  CHECK(!call("pdb_get_vector",{108,x,y,z,-1}) && f(x)==0.0f);
  CHECK(!call("pdb_get_vector",{109,x,y,z,-1}) && f(x)==0.0f);
  CHECK(!call("pdb_get_vector",{102,x,y,z,9999}) && f(x)==0.0f);
  CHECK(call("pdb_json_get_int",{101,201,0})==12);
  CHECK(call("pdb_json_get_str",{101,211,706,16,0}) && output[706]=="hello");
  CHECK(call("pdb_get_vector",{102,x,y,z,0}) && f(z)==13.5f);
  CHECK(!call("pdb_json_get_vector",{101,207,x,y,z,1}) && f(x)==0.0f && f(y)==0.0f && f(z)==0.0f);
  CHECK(!call("pdb_get_vector",{102,x,y,z,-2}) && f(x)==0.0f && f(y)==0.0f && f(z)==0.0f);
  CHECK(!call("pdb_json_get_str",{101,211,706,16,1}) && output[706].empty());
  CHECK(call("pdb_activate_result",{0}));
  CHECK(!call("pdb_json_get_int",{101,201,-1}));
  life.detach(&amx); life.stop();
}
