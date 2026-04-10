x=present
echo "${x:+yes}"
echo "${unset_var:+yes}"
y=
echo "${y:+something}"
