echo "============`date +%F' '%T`==========="
api()
{
  echo "Created building folder: $1"

	scp -P 26122 $1/../drogon-http.zip  root@localhost:~
}

current_dir="${PWD}"

case "$1" in
  api)
    api $current_dir
    ;;
esac

