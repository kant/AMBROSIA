# Perform the code-generation step for this example application.

if ( $env:AMBVARIANTCORE ) {
    $AMBVARIANTCORE=$env:AMBVARIANTCORE
} else {
    $AMBVARIANTCORE = "x64\Debug\netcoreapp2.0"
}


# Build the API projects
& dotnet publish "API\ServerAPI.csproj" -f "netcoreapp2.0"
& dotnet publish "ClientAPI\ClientAPI.csproj" -f "netcoreapp2.0"

# Create the dependencies folder
New-Item -ItemType Directory -Force -Path "CodeGenDependencies\netcoreapp2.0"
Get-ChildItem "CodeGenDependencies\netcoreapp2.0" | Remove-Item
Copy-Item "API\bin\Debug\netcoreapp2.0\publish\*" -Force -Destination "CodeGenDependencies\netcoreapp2.0\"
Copy-Item "ClientAPI\bin\Debug\netcoreapp2.0\publish\*" -Force -Destination "CodeGenDependencies\netcoreapp2.0\"

Write-Host "Using variant of AmbrosiaCS.exe: $env:AMBVARIANTCORE"

Write-Host "Executing codegen command: dotnet ..\..\Clients\CSharp\AmbrosiaCS\bin\$env:AMBVARIANTCORE\AmbrosiaCS.dll CodeGen -a=API\bin\$env:AMBVARIANTCORE\ServerAPI.dll -a=ClientAPI\bin\$env:AMBVARIANTCORE\ClientAPI.dll -o=PTIAmbrosiaGeneratedAPI -f=netcoreapp2.0 -b=CodeGenDependencies\netcoreapp2.0"

# Generate the assemblies, assumes an .exe which is created by a .Net Framework build:
& dotnet "..\..\Clients\CSharp\AmbrosiaCS\bin\$env:AMBVARIANTCORE\AmbrosiaCS.dll" CodeGen -a="API\bin\$env:AMBVARIANTCORE\ServerAPI.dll" -a="ClientAPI\bin\$env:AMBVARIANTCORE\ClientAPI.dll" -o="PTAmbrosiaGeneratedAPINetCore" -f="netcoreapp2.0" -b="CodeGenDependencies\netcoreapp2.0"
