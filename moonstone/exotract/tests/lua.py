import json, subprocess, sys, tempfile
from pathlib import Path
exe, header, bind, resources, cc=sys.argv[1:]
with tempfile.TemporaryDirectory() as tmp:
    script=Path(tmp)/'test.lua'
    # The transport fallback is deliberately external/unsafe, and portable to
    # standalone builds without the ExoML library.
    transport=str(Path(header).parent/'transport.json')
    prefix=f'''
local e=require('exotract')
local config={{path={json.dumps(header)},parser={{main='external',fallback={{e.command.exec({{'cat',{json.dumps(transport)}}})}}}}}}
config.compiler={json.dumps(cc)}
local lib=e.exolib.new('Fixture',config)
'''
    script.write_text(prefix+'''
assert(getmetatable(lib)=='Exotract.Library')
lib:Preprocess():Parse()
local result=lib:Bind({safe_exports='unsafe_explicit'})
assert(result.Internals.unsafe)
local ok=pcall(function() lib:Bind({safe_exports='reviewed_only'}) end)
assert(not ok)
for i=1,100 do
  assert(not pcall(function() e.exolib.new('Bad',{path=42}) end))
  assert(not pcall(function() e.exolib.new('Bad',{path='a',defines={false}}) end))
end
assert(not pcall(function() error({bad=true}) end))
return result.Internals
''')
    result=subprocess.run([exe,'lua',str(script)],capture_output=True,text=True,check=True)
    manifest=json.loads(result.stdout)
    assert manifest['complete'] and manifest['externally_imported'] and not manifest['safe_export']
    script.write_text('error({not_a_string=true})')
    result=subprocess.run([exe,'lua',str(script)],capture_output=True,text=True)
    assert result.returncode != 0 and 'non-string error' in result.stderr
    if bind:
        contract=Path(tmp)/'fixture.exocontract.json'
        contract.write_text(json.dumps({'schema_version':2,'library':'Fixture',
            'headers':[header],'state':'annotated','functions':[{'c_name':'et_create',
            'effects':{'effects':['foreign_call'],'unknown':True}}]}))
        script.write_text(prefix+f'''
config.exoml_bind={json.dumps(bind)}
config.clang_resource_dir={json.dumps(resources)}
local annotated=e.exolib.new('Fixture',config):Parse()
local result=annotated:Bind({{contract={json.dumps(str(contract))}}})
assert(result.Internals.unsafe and type(result.Internals.contract)=='string')
assert(not pcall(function() annotated:Bind({{contract={json.dumps(str(contract))},safe_exports='reviewed_only'}}) end))
return result.Internals.contract
''')
        result=subprocess.run([exe,'lua',str(script)],capture_output=True,text=True,check=True)
        assert json.loads(result.stdout)['state']=='annotated'
        contract.write_text('{"schema_version":2,"library":"bad","functions":[{"c_name":"absent"}]}')
        result=subprocess.run([exe,'lua',str(script)],capture_output=True,text=True)
        assert result.returncode != 0 and not result.stdout
        assert 'EXOML-CONTRACT-DRIFT' in result.stderr
print('Lua execution, type errors, userdata protection and unsafe binding passed')
